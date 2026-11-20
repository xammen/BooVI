#include "veyra/ui/GuestShareModel.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <format>

#include "veyra/Log.h"
#include "veyra/guest/GuestStreamer.h"
#include "veyra/ui/QmlPlayerBridge.h"

namespace veyra::ui {
namespace {

constexpr int kHttpsPort = 8443;   // tailscale serve's https port

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

// The host's maximum (Réglages → partage, and the share panel): what the guest may receive.
guest::Limit limitOf(const QVariantMap& prefs) {
    guest::Limit limit;
    limit.feed = guest::parseFeed(prefs.value(QStringLiteral("guestFeed"), QStringLiteral("120")).toString().toStdString(), guest::Feed::Smooth120);
    const QString size = prefs.value(QStringLiteral("guestResolution"), QStringLiteral("native")).toString();
    limit.size = size == QLatin1String("720p") ? guest::Size::P720 : size == QLatin1String("1080p") ? guest::Size::P1080 : guest::Size::Native;
    limit.kbps = uint32_t(std::clamp(prefs.value(QStringLiteral("guestBitrateMbps"), 50).toInt(), 5, 100)) * 1000;
    return limit;
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
    : QObject(parent), guest_(guest), bridge_(bridge), timer_(new QTimer(this)) {
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &GuestShareModel::poll);
    connect(&bridge_, &QmlPlayerBridge::preferencesChanged, this, &GuestShareModel::applyPreferences);
    guest_.setMode(modeOf(bridge_.preferences().value(QStringLiteral("guestMode")).toString()));
    relay_ = relayName(guest::configuredRelay());
    queryTailscale();
}

QString GuestShareModel::brokerHost() const {
    return QUrl(QString::fromStdString(guest::brokerUrl())).host();
}

GuestShareModel::~GuestShareModel() { stop(); }

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
    if (visiting_) return;   // a friend's visit keeps its own mode and feed until it ends
    const auto prefs = bridge_.preferences();
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

void GuestShareModel::start() {
    if (active_) return;
    const auto prefs = bridge_.preferences();
    shareReach_ = reachPreference();
    relay_ = relayName(guest::configuredRelay());
    guest::ShareSettings settings;
    settings.limit = limitOf(prefs);
    settings.reach = shareReach_ == QLatin1String("broker") ? guest::Reach::Broker : guest::Reach::Tailnet;
    const QString app = QCoreApplication::applicationDirPath();
    settings.webRoot = (app + QStringLiteral("/web/guest")).toStdString();
    settings.fontFile = (app + QStringLiteral("/qml/Veyra/fonts/JetBrainsMono-Regular.ttf")).toStdString();
    // Tests: another port while a second BooVI already shares on 47800.
    if (const int port = qEnvironmentVariableIntValue("VEYRA_GUEST_PORT"); port > 1024 && port < 65536) settings.port = uint16_t(port);
    std::string error;
    if (!guest_.start(settings, error)) {
        error_ = QString::fromStdString(error);
        emit notice(tr("partage impossible : %1").arg(error_), true);
        emit changed();
        return;
    }
    error_.clear();
    active_ = true;
    poll();
    timer_->start();
    queryTailscale();
    emit changed();
}

void GuestShareModel::stop() {
    if (!active_) return;
    if (held_) {
        // The gateway keeps listening for its friends: only the guest and the share link go.
        guest_.kick();
        emit notice(tr("invité coupé · le lien a changé · l'accueil des invités reste ouvert"), false);
        QTimer::singleShot(300, this, &GuestShareModel::poll);
        return;
    }
    guest_.stop();
    active_ = false;
    timer_->stop();
    token_.clear();
    phase_ = QStringLiteral("off");
    broker_ = QStringLiteral("off");
    publicUrl_.clear();
    linkWritten_.clear();
    stats_.clear();
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

void GuestShareModel::kick() {
    if (!active_) return;
    guest_.kick();
    emit notice(tr("invité coupé · le lien a changé"), false);
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
    const QString phase = phaseName(s.phase);
    const bool connectedNow = phase == QLatin1String("connected") && phase_ != QLatin1String("connected");
    if (connectedNow) emit notice(tr("un invité a rejoint le stream"), false);
    else if (phase_ == QLatin1String("connected") && phase != QLatin1String("connected") && active_) emit notice(tr("l'invité est parti"), false);
    phase_ = active_ ? phase : QStringLiteral("off");
    accessName_ = QString::fromStdString(s.accessName);
    token_ = QString::fromStdString(s.token);
    port_ = s.port;
    broker_ = brokerName(s.broker);
    publicUrl_ = QString::fromStdString(s.publicUrl);
    if (!s.tailscaleIp.empty()) tailscaleIp_ = QString::fromStdString(s.tailscaleIp);
    if (!s.error.empty()) error_ = QString::fromStdString(s.error);
    auto ms = [](double v) { return v < 0 ? QVariant() : QVariant(v); };
    stats_ = QVariantMap{
        {QStringLiteral("rtt"), ms(s.rttMs)}, {QStringLiteral("mbps"), s.sentMbps}, {QStringLiteral("fps"), s.fps},
        {QStringLiteral("width"), s.width}, {QStringLiteral("height"), s.height}, {QStringLiteral("kbps"), s.targetKbps},
        {QStringLiteral("path"), pathLabel(QString::fromStdString(s.remote), s.relayed)},
        {QStringLiteral("encoder"), QString::fromStdString(s.encoder)}, {QStringLiteral("inputs"), qulonglong(s.inputs)},
        {QStringLiteral("pad"), s.padConnected}, {QStringLiteral("synthetic"), s.synthetic},
        {QStringLiteral("padName"), QString::fromStdString(s.padName)}, {QStringLiteral("padMapping"), QString::fromStdString(s.padMapping)},
        {QStringLiteral("padSilent"), s.padSilent},
        {QStringLiteral("client"), s.nativeClient ? QStringLiteral("boovi") : QStringLiteral("browser")},
        {QStringLiteral("access"), QString::fromStdString(s.accessName)},
        {QStringLiteral("presentToSent"), ms(s.presentToSentMs)}, {QStringLiteral("encode"), ms(s.encodeMs)},
        {QStringLiteral("presentToDisplay"), ms(s.presentToDisplayMs)}, {QStringLiteral("receiveToDisplay"), ms(s.receiveToDisplayMs)},
        {QStringLiteral("decode"), ms(s.decodeMs)}, {QStringLiteral("inputToHost"), ms(s.inputToHostMs)},
        {QStringLiteral("inputToPs5"), ms(s.inputToPs5Ms)}, {QStringLiteral("loss"), s.lossPercent},
        {QStringLiteral("decodedToSent"), ms(s.decodedToSentMs)},
        {QStringLiteral("decodedToSentReal"), ms(s.decodedToSentRealMs)}, {QStringLiteral("decodedToSentSmooth"), ms(s.decodedToSentSmoothMs)},
        {QStringLiteral("feed"), QString::fromLatin1(guest::feedName(s.quality.feed))}, {QStringLiteral("size"), QString::fromLatin1(guest::sizeName(s.quality.size))},
        {QStringLiteral("auto"), s.automatic}, {QStringLiteral("sourceHeight"), s.sourceHeight},
        {QStringLiteral("link"), s.linkKbps}, {QStringLiteral("linkRtt"), ms(s.linkRttMs)}, {QStringLiteral("linkLoss"), s.linkLossPercent},
        {QStringLiteral("hz"), s.displayHz > 0 ? QVariant(s.displayHz) : QVariant()}, {QStringLiteral("guestFps"), s.guestFps >= 0 ? QVariant(s.guestFps) : QVariant()}};
    if (++ticks_ % 10 == 0 && active_ && shareReach_ == QLatin1String("tailnet")) queryTailscale();   // every 5 s: serve and the peer's path
    // Tests: the link goes to a file the harness reads (never to the log), once it is known.
    if (const QString file = qEnvironmentVariable("VEYRA_GUEST_LINK_FILE"); !file.isEmpty() && active_ && !link().isEmpty() && link() != linkWritten_) {
        linkWritten_ = link();
        QFile out(file);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write((localLink() + QLatin1Char('\n') + linkWritten_ + QLatin1Char('\n')).toUtf8());
    }
    emit changed();
}

} // namespace veyra::ui
