#include "veyra/ui/GuestShareModel.h"

#include <windows.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <format>

#include "veyra/Log.h"
#include "veyra/guest/GuestNotify.h"
#include "veyra/guest/GuestStreamer.h"
#include "veyra/ui/DiscordNotifier.h"
#include "veyra/ui/Dpapi.h"
#include "veyra/ui/QmlPlayerBridge.h"

namespace veyra::ui {
namespace {

constexpr int kHttpsPort = 8443;              // tailscale serve's https port
constexpr int kPlays = 200;                   // play history kept
constexpr qint64 kRestartEveryMs = 60'000;    // the watchdog restarts a stuck share at most this often

QString tailscaleExe() {
    const QString found = QStandardPaths::findExecutable(QStringLiteral("tailscale"));
    if (!found.isEmpty()) return found;
    const QString installed = QStringLiteral("C:/Program Files/Tailscale/tailscale.exe");
    return QFile::exists(installed) ? installed : QString();
}

guest::ControlMode modeOf(const QString& name) {
    if (name == QLatin1String("together")) return guest::ControlMode::Together;
    if (name == QLatin1String("watch")) return guest::ControlMode::WatchOnly;
    return guest::ControlMode::GuestPlays;
}

QString brokerName(guest::BrokerPhase phase) {
    switch (phase) {
    case guest::BrokerPhase::Connecting: return QStringLiteral("connecting");
    case guest::BrokerPhase::Connected: return QStringLiteral("connected");
    default: return QStringLiteral("off");
    }
}

QString relayName(guest::Relay relay) {
    return relay == guest::Relay::Owner ? QStringLiteral("owner") : relay == guest::Relay::OwnKey ? QStringLiteral("key") : QStringLiteral("none");
}

QString text(std::string_view s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }

// The host's maximum (Réglages → partage, and the share panel): what a viewer may receive, and
// the host's whole upload for the share.
guest::Limit limitOf(const QVariantMap& prefs) {
    guest::Limit limit;
    limit.feed = guest::parseFeed(prefs.value(QStringLiteral("guestFeed"), QStringLiteral("120")).toString().toStdString(), guest::Feed::Smooth120);
    const QString size = prefs.value(QStringLiteral("guestResolution"), QStringLiteral("native")).toString();
    limit.size = size == QLatin1String("720p") ? guest::Size::P720 : size == QLatin1String("1080p") ? guest::Size::P1080 : guest::Size::Native;
    limit.kbps = uint32_t(std::clamp(prefs.value(QStringLiteral("guestBitrateMbps"), 50).toInt(), 5, 100)) * 1000;
    return limit;
}

// The room's rules (Réglages → partage → à plusieurs).
guest::RoomRules rulesOf(const QVariantMap& prefs) {
    guest::RoomRules r;
    r.room.maxMembers = std::clamp(prefs.value(QStringLiteral("guestMaxViewers"), 4).toInt(), 1, int(6));
    r.room.turnUs = int64_t(std::clamp(prefs.value(QStringLiteral("guestTurnMinutes"), 0).toInt(), 0, 240)) * 60'000'000;
    r.room.diceAllowed = prefs.value(QStringLiteral("guestDice"), true).toBool();
    r.idleMinutes = std::clamp(prefs.value(QStringLiteral("gatewayIdleMinutes"), 20).toInt(), 0, 240);
    r.ownerPriority = prefs.value(QStringLiteral("guestOwnerPriority"), true).toBool();
    r.historyToGuests = prefs.value(QStringLiteral("guestHistoryToGuests"), false).toBool();
    r.defaultSeat = prefs.value(QStringLiteral("guestDefaultSeat")).toString() == QLatin1String("watcher") ? guest::Seat::Watcher : guest::Seat::Player;
    return r;
}

uint32_t noticeMask(const QVariantMap& prefs) {
    const QVariant v = prefs.value(QStringLiteral("discordEvents"));
    return v.isValid() ? uint32_t(v.toInt()) & guest::kAllNotices : guest::kDefaultNotices;
}

QString phaseName(guest::Phase phase) {
    switch (phase) {
    case guest::Phase::Waiting: return QStringLiteral("waiting");
    case guest::Phase::Connecting: return QStringLiteral("connecting");
    case guest::Phase::Connected: return QStringLiteral("connected");
    default: return QStringLiteral("off");
    }
}

} // namespace

GuestShareModel::GuestShareModel(guest::GuestStreamer& guest, QmlPlayerBridge& bridge, QObject* parent)
    : QObject(parent), guest_(guest), bridge_(bridge), discord_(new DiscordNotifier(this)), timer_(new QTimer(this)) {
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &GuestShareModel::poll);
    connect(&bridge_, &QmlPlayerBridge::preferencesChanged, this, &GuestShareModel::applyPreferences);
    connect(discord_, &DiscordNotifier::changed, this, &GuestShareModel::changed);
    guest_.setMode(modeOf(bridge_.preferences().value(QStringLiteral("guestMode")).toString()));
    discord_->setEvents(noticeMask(bridge_.preferences()));
    relay_ = relayName(guest::configuredRelay());
    loadPlays();
    queryTailscale();
    testScript();
}

// Tests: VEYRA_TEST_ROOM="give:1@20,seat:2:watcher@25,kick:2@30,take@35,hand@40,roll@45,renew@50"
// runs the host's room commands at those seconds after start, on the viewer at that index in the
// room's order, as the panel's buttons would.
void GuestShareModel::testScript() {
    const QString script = qEnvironmentVariable("VEYRA_TEST_ROOM");
    for (const QString& step : script.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList at = step.split(QLatin1Char('@'));
        if (at.size() != 2) continue;
        const QStringList parts = at[0].split(QLatin1Char(':'));
        QTimer::singleShot(int(at[1].toDouble() * 1000), this, [this, parts] {
            auto tagAt = [this](int i) { return i >= 0 && i < viewers_.size() ? viewers_[i].toMap().value(QStringLiteral("tag")).toString() : QString(); };
            const QString cmd = parts.value(0);
            const QString tag = tagAt(parts.value(1).toInt());
            veyra::log::info("guest-room", "test: " + parts.join(QLatin1Char(':')).toStdString());
            if (cmd == QLatin1String("give")) give(tag);
            else if (cmd == QLatin1String("seat")) setSeat(tag, parts.value(2));
            else if (cmd == QLatin1String("kick")) kick(tag);
            else if (cmd == QLatin1String("take")) takeBack();
            else if (cmd == QLatin1String("hand")) handBack();
            else if (cmd == QLatin1String("roll")) roll();
            else if (cmd == QLatin1String("renew")) renewLink();
        });
    }
}

QString GuestShareModel::brokerHost() const {
    return QUrl(QString::fromStdString(guest::brokerUrl())).host();
}

GuestShareModel::~GuestShareModel() { held_ = false; stop(); keepAwake(false); }

QString GuestShareModel::localLink() const {
    if (!active_ || token_.isEmpty() || !port_) return {};
    return QStringLiteral("http://127.0.0.1:%1/#t=%2").arg(port_).arg(token_);
}

// The link to give the friend: the room's on the broker; in the tailnet, https through Tailscale
// when it is on, the tailnet address otherwise.
QString GuestShareModel::link() const {
    if (!active_ || token_.isEmpty()) return {};
    if (shareReach_ == QLatin1String("broker")) return publicUrl_.isEmpty() ? QString() : QStringLiteral("%1#t=%2").arg(publicUrl_, token_);
    if (httpsReady_ && !dnsName_.isEmpty()) return QStringLiteral("https://%1:%2/#t=%3").arg(dnsName_).arg(kHttpsPort).arg(token_);
    if (!tailscaleIp_.isEmpty()) return QStringLiteral("http://%1:%2/#t=%3").arg(tailscaleIp_).arg(port_).arg(token_);
    return localLink();
}

QString GuestShareModel::reach() const {
    if (active_ && shareReach_ == QLatin1String("broker")) return shareReach_;
    if (httpsReady_) return QStringLiteral("tailnet-https");
    if (!tailscaleIp_.isEmpty()) return QStringLiteral("tailnet-http");
    return QStringLiteral("local");
}


QString GuestShareModel::mode() const {
    switch (guest_.mode()) {
    case guest::ControlMode::Together: return QStringLiteral("together");
    case guest::ControlMode::WatchOnly: return QStringLiteral("watch");
    default: return QStringLiteral("guest");
    }
}

void GuestShareModel::applyPreferences() {
    const auto prefs = bridge_.preferences();
    discord_->setEvents(noticeMask(prefs));
    if (active_) guest_.setRules(rulesOf(prefs));
    if (visiting_) return;   // a friend's visit keeps its own mode and feed until it ends
    if (const auto m = prefs.value(QStringLiteral("guestMode")).toString(); !m.isEmpty() && m != mode()) { guest_.setMode(modeOf(m)); emit changed(); }
    if (active_) guest_.setLimit(limitOf(prefs));
}

void GuestShareModel::setHeld(bool held) {
    if (held_ == held) return;
    held_ = held;
    emit changed();
}

void GuestShareModel::applyVisit(guest::ControlMode mode, guest::Feed feedCap) {
    visiting_ = true;
    guest_.setMode(mode);
    auto limit = limitOf(bridge_.preferences());
    limit.feed = std::min(limit.feed, feedCap);
    if (active_) guest_.setLimit(limit);
    emit changed();
}

void GuestShareModel::endVisit() {
    if (!visiting_) return;
    visiting_ = false;
    guest_.setMode(modeOf(bridge_.preferences().value(QStringLiteral("guestMode")).toString()));
    if (active_) guest_.setLimit(limitOf(bridge_.preferences()));
    emit changed();
}

void GuestShareModel::dropAccess(const QString& accessId) { guest_.dropAccess(accessId.toStdString()); }

QStringList GuestShareModel::accessIds() const {
    QStringList out;
    for (const auto& v : viewers_) {
        const auto m = v.toMap();
        if (m.value(QStringLiteral("connected")).toBool() && !m.value(QStringLiteral("access")).toString().isEmpty()) out << m.value(QStringLiteral("access")).toString();
    }
    return out;
}

QString GuestShareModel::accessLink(const QString& token) const {
    if (reachPreference() == QLatin1String("broker")) {
        const QString room = QString::fromStdString(guest::brokerRoomLink());
        return room.isEmpty() ? QString() : QStringLiteral("%1#a=%2").arg(room, token);
    }
    const int port = active_ && port_ ? port_ : 47800;
    if (httpsReady_ && !dnsName_.isEmpty()) return QStringLiteral("https://%1:%2/#a=%3").arg(dnsName_).arg(kHttpsPort).arg(token);
    if (!tailscaleIp_.isEmpty()) return QStringLiteral("http://%1:%2/#a=%3").arg(tailscaleIp_).arg(port).arg(token);
    return QStringLiteral("http://127.0.0.1:%1/#a=%2").arg(port).arg(token);
}

// The broker by default when this build has one. Tests set VEYRA_GUEST_REACH
// (scripts\fork\guest-latency.ps1 sets tailnet).
QString GuestShareModel::reachPreference() const {
    const QString forced = qEnvironmentVariable("VEYRA_GUEST_REACH");
    const QString wanted = forced.isEmpty() ? bridge_.preferences().value(QStringLiteral("guestReach")).toString() : forced;
    const bool broker = !guest::brokerUrl().empty();
    if (wanted == QLatin1String("tailnet") || (wanted == QLatin1String("broker") && broker)) return wanted;
    return broker ? QStringLiteral("broker") : QStringLiteral("tailnet");
}


bool GuestShareModel::startStreamer() {
    const auto prefs = bridge_.preferences();
    shareReach_ = reachPreference();
    relay_ = relayName(guest::configuredRelay());
    guest::ShareSettings settings;
    settings.limit = limitOf(prefs);
    settings.rules = rulesOf(prefs);
    settings.reach = shareReach_ == QLatin1String("broker") ? guest::Reach::Broker : guest::Reach::Tailnet;
    const QString app = QCoreApplication::applicationDirPath();
    settings.webRoot = (app + QStringLiteral("/web/guest")).toStdString();
    settings.fontFile = (app + QStringLiteral("/qml/Veyra/fonts/JetBrainsMono-Regular.ttf")).toStdString();
    // Tests: another port while a second BooVI already shares on 47800.
    if (const int port = qEnvironmentVariableIntValue("VEYRA_GUEST_PORT"); port > 1024 && port < 65536) settings.port = uint16_t(port);
    std::string error;
    if (!guest_.start(settings, error)) {
        error_ = QString::fromStdString(error);
        return false;
    }
    error_.clear();
    pushHistory();
    return true;
}

void GuestShareModel::start() {
    if (active_) return;
    if (!startStreamer()) {
        emit notice(tr("partage impossible : %1").arg(error_), true);
        emit changed();
        return;
    }
    active_ = true;
    stuckSaid_ = false;
    discord_->notify(guest::Notice::ShareStarted, guest::noticeLine(guest::Notice::ShareStarted));
    poll();
    timer_->start();
    queryTailscale();
    emit changed();
}

void GuestShareModel::stop() {
    if (!active_) return;
    if (held_) {
        // The gateway keeps listening for its friends: only the share link and its guests go.
        guest_.renewLink();
        emit notice(tr("le lien a changé · ses invités sont partis · l'accueil des invités reste ouvert"), false);
        QTimer::singleShot(300, this, &GuestShareModel::poll);
        return;
    }
    guest_.stop();
    drainRoom();
    keepAwake(false);
    discord_->notify(guest::Notice::ShareStopped, guest::noticeLine(guest::Notice::ShareStopped));
    active_ = false;
    timer_->stop();
    token_.clear();
    phase_ = QStringLiteral("off");
    broker_ = QStringLiteral("off");
    publicUrl_.clear();
    linkWritten_.clear();
    viewers_.clear();
    room_.clear();
    sentMbps_ = 0;
    // What this session opened in Tailscale closes with it, also when the app is closing
    // (detached, so quitting does not kill the command).
    const QString exe = tailscaleExe();
    if (!exe.isEmpty() && weServed_) {
        QProcess::startDetached(exe, {QStringLiteral("serve"), QStringLiteral("--https=%1").arg(kHttpsPort), QStringLiteral("off")});
        veyra::log::info("guest-tailscale", "serve off (sharing stopped)");
    }
    weServed_ = false;
    httpsReady_ = false;
    emit changed();
}

void GuestShareModel::kick(const QString& tag) {
    if (!active_) return;
    for (const auto& v : viewers_)
        if (const auto m = v.toMap(); m.value(QStringLiteral("tag")).toString() == tag && !m.value(QStringLiteral("access")).toString().isEmpty())
            emit friendCut(m.value(QStringLiteral("access")).toString());
    guest_.kick(tag.toStdString());
    QTimer::singleShot(300, this, &GuestShareModel::poll);
}

void GuestShareModel::renewLink() {
    if (!active_) return;
    guest_.renewLink();
    emit notice(tr("nouveau lien · les invités du lien précédent sont partis"), false);
    QTimer::singleShot(300, this, &GuestShareModel::poll);
}

void GuestShareModel::setMode(const QString& mode) {
    bridge_.setPreference(QStringLiteral("guestMode"), mode);
    guest_.setMode(modeOf(mode));
    emit changed();
}

void GuestShareModel::copyLink() {
    if (auto* clipboard = QGuiApplication::clipboard(); clipboard && !link().isEmpty()) {
        clipboard->setText(link());
        emit notice(tr("lien copié"), false);
    }
}

void GuestShareModel::give(const QString& tag) { guest_.give(tag.toStdString()); QTimer::singleShot(150, this, &GuestShareModel::poll); }
void GuestShareModel::takeBack() { guest_.takeBack(); QTimer::singleShot(150, this, &GuestShareModel::poll); }
void GuestShareModel::handBack() { guest_.handBack(); QTimer::singleShot(150, this, &GuestShareModel::poll); }
void GuestShareModel::roll() { guest_.roll(); QTimer::singleShot(150, this, &GuestShareModel::poll); }
void GuestShareModel::setSeat(const QString& tag, const QString& seat) {
    if (const auto s = guest::parseSeat(seat.toStdString())) guest_.setSeat(tag.toStdString(), *s);
    QTimer::singleShot(150, this, &GuestShareModel::poll);
}

// ---- Discord ---------------------------------------------------------------------------------

QVariantMap GuestShareModel::discord() const {
    const uint32_t mask = noticeMask(bridge_.preferences());
    QVariantMap events;
    for (int i = 0; i < 9; ++i) events.insert(text(guest::noticeName(guest::Notice(i))), bool(mask & (1u << i)));
    return {{"configured", discord_->configured()}, {"status", discord_->status()}, {"events", events}};
}

bool GuestShareModel::setDiscordWebhook(const QString& url) {
    QString why;
    if (!discord_->setWebhook(url, why)) { emit notice(why, true); return false; }
    emit notice(tr("webhook discord enregistré"), false);
    emit changed();
    return true;
}

void GuestShareModel::clearDiscordWebhook() { discord_->clear(); emit changed(); }
void GuestShareModel::testDiscord() { discord_->test(); }

void GuestShareModel::setDiscordEvent(const QString& name, bool on) {
    uint32_t mask = noticeMask(bridge_.preferences());
    for (int i = 0; i < 9; ++i)
        if (text(guest::noticeName(guest::Notice(i))) == name) mask = on ? mask | (1u << i) : mask & ~(1u << i);
    bridge_.setPreference(QStringLiteral("discordEvents"), int(mask));
}

// ---- the play history --------------------------------------------------------------------------

void GuestShareModel::loadPlays() {
    QFile file(QDir(bridge_.dataDirectory()).filePath(QStringLiteral("guest-plays.dpapi")));
    if (!file.open(QIODevice::ReadOnly)) return;
    QByteArray plain = dpapiUnseal(file.readAll());
    for (const auto& value : QJsonDocument::fromJson(plain).object().value(QStringLiteral("plays")).toArray()) {
        const auto o = value.toObject();
        plays_.push_back({o.value(QStringLiteral("name")).toString(), qint64(o.value(QStringLiteral("at")).toDouble()), qint64(o.value(QStringLiteral("s")).toDouble())});
    }
    plain.fill('\0');
}

void GuestShareModel::savePlays() {
    QJsonArray list;
    for (const auto& p : plays_) list.push_back(QJsonObject{{"name", p.name}, {"at", double(p.at)}, {"s", double(p.seconds)}});
    const QByteArray sealed = dpapiSeal(QJsonDocument(QJsonObject{{"version", 1}, {"plays", list}}).toJson(QJsonDocument::Compact));
    QSaveFile file(QDir(bridge_.dataDirectory()).filePath(QStringLiteral("guest-plays.dpapi")));
    if (sealed.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(sealed) != sealed.size() || !file.commit())
        veyra::log::warn("guest-room", "the play history could not be saved");
}

QVariantList GuestShareModel::plays() const {
    QVariantList out;
    for (auto it = plays_.rbegin(); it != plays_.rend(); ++it)
        out.push_back(QVariantMap{{"name", it->name}, {"at", double(it->at) * 1000}, {"seconds", double(it->seconds)}});
    return out;
}

void GuestShareModel::clearPlays() { plays_.clear(); savePlays(); pushHistory(); emit changed(); }

// The guests' view of the last ten plays (shown on their page when the host allows it).
void GuestShareModel::pushHistory() {
    std::vector<guest::PlayRecord> last;
    for (auto it = plays_.rbegin(); it != plays_.rend() && last.size() < 10; ++it) last.push_back({it->name.toStdString(), it->at, it->seconds});
    guest_.setHistory(std::move(last));
}

// The room's events: the history, the app's notices and Discord.
void GuestShareModel::drainRoom() {
    bool played = false;
    for (const auto& e : guest_.drainEvents()) {
        const QString name = QString::fromStdString(e.name);
        switch (e.kind) {
        case guest::RoomEvent::Kind::Joined: emit notice(tr("%1 a rejoint le stream").arg(name), false); break;
        case guest::RoomEvent::Kind::Left: emit notice(tr("%1 est parti").arg(name), false); break;
        case guest::RoomEvent::Kind::PadReleased:
            if (e.heldUs >= 5'000'000) {
                plays_.push_back({name, QDateTime::currentSecsSinceEpoch() - e.heldUs / 1'000'000, e.heldUs / 1'000'000});
                played = true;
            }
            break;
        case guest::RoomEvent::Kind::DiceRolled: emit notice(tr("dés : %1 a la main").arg(name), false); break;
        default: break;
        }
        if (auto line = guest::noticeFor(e)) discord_->notify(line->kind, std::move(line->text));
    }
    if (played) {
        if (plays_.size() > size_t(kPlays)) plays_.erase(plays_.begin(), plays_.end() - kPlays);
        savePlays();
        pushHistory();
    }
}

// While anyone watches, Windows does not put the PC to sleep (the screen may still turn off).
void GuestShareModel::keepAwake(bool on) {
    if (on == awake_) return;
    awake_ = on;
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
    veyra::log::info("guest", on ? "keeping the PC awake while guests watch" : "the PC may sleep again");
}

// A share stuck on a request past its deadline (GuestBroker.cpp), with nobody connected, is
// restarted: the room reopens with the same link for friends' accesses, a new share token. A
// control loop that stopped cannot be restarted safely (stop() would wait on it): Discord hears.
void GuestShareModel::watchdog(bool stuck, bool controlAlive, bool anyone) {
    if (!stuck) { stuckSaid_ = false; return; }
    if (!stuckSaid_) {
        stuckSaid_ = true;
        discord_->notify(guest::Notice::ShareStuck, guest::noticeLine(guest::Notice::ShareStuck));
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!controlAlive || anyone || now - restartedAtMs_ < kRestartEveryMs) return;
    restartedAtMs_ = now;
    veyra::log::warn("guest", "watchdog: the share is stuck with nobody connected; restarting it");
    guest_.stop();
    drainRoom();
    if (!startStreamer()) { veyra::log::error("guest", "watchdog: the share did not restart"); emit notice(tr("partage impossible : %1").arg(error_), true); }
    else veyra::log::info("guest", "watchdog: share restarted");
}

void GuestShareModel::enableHttps() {
    if (!active_ || busy_) return;
    weServed_ = true;
    runTailscale({QStringLiteral("serve"), QStringLiteral("--bg"), QStringLiteral("--https=%1").arg(kHttpsPort), QStringLiteral("http://127.0.0.1:%1").arg(port_)},
                 QStringLiteral("serve on"));
}

void GuestShareModel::disableHttps() {
    if (busy_ || !httpsReady_) return;
    runTailscale({QStringLiteral("serve"), QStringLiteral("--https=%1").arg(kHttpsPort), QStringLiteral("off")}, QStringLiteral("serve off"));
    weServed_ = false;
}

void GuestShareModel::runTailscale(const QStringList& args, const QString& what) {
    const QString exe = tailscaleExe();
    if (exe.isEmpty()) { emit notice(tr("tailscale n'est pas installé"), true); return; }
    busy_ = true;
    emit changed();
    auto* process = new QProcess(this);
    process->setProgram(exe);
    process->setArguments(args);
    connect(process, &QProcess::finished, this, [this, process, what](int code, QProcess::ExitStatus) {
        // Only the outcome is logged: tailscale's own messages can name the tailnet.
        veyra::log::info("guest-tailscale", std::format("{} exit={}", what.toStdString(), code));
        if (code != 0) {
            const QString detail = QString::fromUtf8(process->readAllStandardError()).trimmed().section(QLatin1Char('\n'), 0, 0);
            emit notice(tr("tailscale a refusé : %1").arg(detail.isEmpty() ? QString::number(code) : detail), true);
        }
        busy_ = false;
        process->deleteLater();
        queryTailscale();
    });
    process->start();
}

// Read-only: the machine's tailnet name, how each peer is reached, and what `serve` exposes.
void GuestShareModel::queryTailscale() {
    const QString exe = tailscaleExe();
    if (exe.isEmpty()) return;
    auto* status = new QProcess(this);
    connect(status, &QProcess::finished, this, [this, status](int code, QProcess::ExitStatus) {
        status->deleteLater();
        if (code != 0) return;
        const auto doc = QJsonDocument::fromJson(status->readAllStandardOutput()).object();
        dnsName_ = doc.value(QStringLiteral("Self")).toObject().value(QStringLiteral("DNSName")).toString();
        if (dnsName_.endsWith(QLatin1Char('.'))) dnsName_.chop(1);
        const auto self = doc.value(QStringLiteral("Self")).toObject().value(QStringLiteral("TailscaleIPs")).toArray();
        tailscaleIp_.clear();
        for (const auto& ip : self) if (!ip.toString().contains(QLatin1Char(':'))) { tailscaleIp_ = ip.toString(); break; }
        peers_.clear();
        const auto peers = doc.value(QStringLiteral("Peer")).toObject();
        for (auto it = peers.begin(); it != peers.end(); ++it) {
            const auto peer = it.value().toObject();
            const QString path = !peer.value(QStringLiteral("CurAddr")).toString().isEmpty() ? QStringLiteral("direct")
                               : QStringLiteral("relayé · derp %1").arg(peer.value(QStringLiteral("Relay")).toString());
            for (const auto& ip : peer.value(QStringLiteral("TailscaleIPs")).toArray()) peers_.insert(ip.toString(), path);
        }
        emit changed();
    });
    status->start(exe, {QStringLiteral("status"), QStringLiteral("--json")});
    if (!active_) return;
    auto* serve = new QProcess(this);
    connect(serve, &QProcess::finished, this, [this, serve](int code, QProcess::ExitStatus) {
        serve->deleteLater();
        if (code != 0) return;
        const auto doc = QJsonDocument::fromJson(serve->readAllStandardOutput()).object();
        const QString host = QStringLiteral("%1:%2").arg(dnsName_).arg(kHttpsPort);
        const QString proxy = doc.value(QStringLiteral("Web")).toObject().value(host).toObject().value(QStringLiteral("Handlers")).toObject()
                                  .value(QStringLiteral("/")).toObject().value(QStringLiteral("Proxy")).toString();
        httpsReady_ = !dnsName_.isEmpty() && proxy.endsWith(QStringLiteral(":%1").arg(port_));
        emit changed();
    });
    serve->start(exe, {QStringLiteral("serve"), QStringLiteral("status"), QStringLiteral("--json")});
}

QString GuestShareModel::pathLabel(const QString& remote, bool relayed) const {
    if (remote.isEmpty()) return {};
    if (relayed) return tr("relais cloudflare");
    if (remote == QLatin1String("127.0.0.1") || remote == tailscaleIp_) return tr("local");
    if (peers_.contains(remote)) return peers_.value(remote).toString();
    return tr("direct");
}


void GuestShareModel::poll() {
    const auto s = guest_.status();
    drainRoom();
    phase_ = active_ ? phaseName(s.phase) : QStringLiteral("off");
    token_ = QString::fromStdString(s.token);
    port_ = s.port;
    broker_ = brokerName(s.broker);
    publicUrl_ = QString::fromStdString(s.publicUrl);
    if (!s.tailscaleIp.empty()) tailscaleIp_ = QString::fromStdString(s.tailscaleIp);
    error_ = QString::fromStdString(s.error);
    sentMbps_ = s.sentMbps;
    playIdleSeconds_ = s.playIdleSeconds;
    auto ms = [](double v) { return v < 0 ? QVariant() : QVariant(v); };
    std::map<std::string, QString> names;
    viewers_.clear();
    bool anyone = false;
    for (const auto& v : s.viewers) {
        names[v.tag] = QString::fromStdString(v.name);
        anyone |= v.connected;
        viewers_.push_back(QVariantMap{
            {QStringLiteral("tag"), QString::fromStdString(v.tag)}, {QStringLiteral("name"), QString::fromStdString(v.name)},
            {QStringLiteral("access"), QString::fromStdString(v.accessId)}, {QStringLiteral("connected"), v.connected},
            {QStringLiteral("seat"), text(guest::seatName(v.seat))}, {QStringLiteral("holder"), v.holder}, {QStringLiteral("queue"), v.queuePosition},
            {QStringLiteral("rtt"), ms(v.rttMs)}, {QStringLiteral("mbps"), v.sentMbps}, {QStringLiteral("fps"), v.fps},
            {QStringLiteral("width"), v.width}, {QStringLiteral("height"), v.height}, {QStringLiteral("kbps"), v.targetKbps}, {QStringLiteral("share"), v.shareKbps},
            {QStringLiteral("path"), pathLabel(QString::fromStdString(v.remote), v.relayed)},
            {QStringLiteral("encoder"), QString::fromStdString(v.encoder)}, {QStringLiteral("inputs"), qulonglong(v.inputs)},
            {QStringLiteral("pad"), v.padConnected}, {QStringLiteral("synthetic"), v.synthetic},
            {QStringLiteral("padName"), QString::fromStdString(v.padName)}, {QStringLiteral("padMapping"), QString::fromStdString(v.padMapping)},
            {QStringLiteral("padSilent"), v.padSilent}, {QStringLiteral("idle"), v.idleSeconds},
            {QStringLiteral("client"), v.nativeClient ? QStringLiteral("boovi") : QStringLiteral("browser")},
            {QStringLiteral("presentToSent"), ms(v.presentToSentMs)}, {QStringLiteral("encode"), ms(v.encodeMs)},
            {QStringLiteral("presentToDisplay"), ms(v.presentToDisplayMs)}, {QStringLiteral("receiveToDisplay"), ms(v.receiveToDisplayMs)},
            {QStringLiteral("decode"), ms(v.decodeMs)}, {QStringLiteral("inputToHost"), ms(v.inputToHostMs)},
            {QStringLiteral("inputToPs5"), ms(v.inputToPs5Ms)}, {QStringLiteral("loss"), v.lossPercent},
            {QStringLiteral("decodedToSent"), ms(v.decodedToSentMs)},
            {QStringLiteral("feed"), text(guest::feedName(v.quality.feed))}, {QStringLiteral("size"), text(guest::sizeName(v.quality.size))},
            {QStringLiteral("auto"), v.automatic}, {QStringLiteral("sourceHeight"), v.sourceHeight},
            {QStringLiteral("link"), v.linkKbps}, {QStringLiteral("linkRtt"), ms(v.linkRttMs)}, {QStringLiteral("linkLoss"), v.linkLossPercent},
            {QStringLiteral("hz"), v.displayHz > 0 ? QVariant(v.displayHz) : QVariant()}, {QStringLiteral("guestFps"), v.guestFps >= 0 ? QVariant(v.guestFps) : QVariant()}});
    }
    QStringList queue;
    for (const auto& tag : s.room.queue) queue << names[tag];
    QVariantMap dice;
    if (const auto& d = s.room.dice) {
        QVariantList rolls;
        for (const auto& t : d->rounds.back()) rolls.push_back(QVariantMap{{"name", names.count(t.id) ? names[t.id] : QString()}, {"value", t.value}});
        dice = {{"n", int(d->n)}, {"winner", names.count(d->winner) ? names[d->winner] : QString()}, {"age", s.room.diceAgeSeconds}, {"rolls", rolls},
                {"by", d->by.empty() ? QString() : names[d->by]}};
    }
    room_ = QVariantMap{{"holder", QString::fromStdString(s.room.holder)}, {"holderName", s.room.holder.empty() ? QString() : names[s.room.holder]},
                        {"held", s.room.heldSeconds}, {"owner", s.room.ownerHolds}, {"queue", queue}, {"dice", dice},
                        {"turn", int(s.room.rules.room.turnUs / 60'000'000)}, {"diceAllowed", s.room.rules.room.diceAllowed},
                        {"max", s.room.rules.room.maxMembers}, {"count", int(s.viewers.size())},
                        {"upload", s.limit.kbps}};
    // Tests of a stuck teardown: the UI thread says it still polls the share, every 2 s.
    if (ticks_ % 4 == 0 && qEnvironmentVariableIsSet("VEYRA_TEST_GUEST_STALL_TEARDOWN"))
        veyra::log::info("guest", std::format("test: ui poll alive viewers={} phase={}", s.viewers.size(), phase_.toStdString()));
    if (active_) {
        keepAwake(anyone);
        watchdog(s.stuck, s.controlAlive, anyone);
    }
    if (++ticks_ % 10 == 0 && active_ && shareReach_ == QLatin1String("tailnet")) queryTailscale();   // every 5 s: serve and the peers' path
    // Tests: the link goes to a file the harness reads (never to the log), once it is known.
    if (const QString file = qEnvironmentVariable("VEYRA_GUEST_LINK_FILE"); !file.isEmpty() && active_ && !link().isEmpty() && link() != linkWritten_) {
        linkWritten_ = link();
        QFile out(file);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write((localLink() + QLatin1Char('\n') + linkWritten_ + QLatin1Char('\n')).toUtf8());
    }
    emit changed();
}

} // namespace veyra::ui
