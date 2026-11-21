#include "veyra/ui/GuestAccessModel.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QMenu>
#include <QSaveFile>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QTime>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <format>

#include "veyra/Log.h"
#include "veyra/guest/GuestStreamer.h"
#include "veyra/sink/RecordingAudioTap.h"
#include "veyra/ui/Dpapi.h"
#include "veyra/ui/GuestShareModel.h"
#include "veyra/ui/QmlPlayerBridge.h"

namespace veyra::ui {
namespace {

constexpr qint64 kAskMs = 60'000;          // the host's answer, then refused
constexpr qint64 kAbandonUs = 20'000'000;  // a page that stopped knocking before it got in
constexpr qint64 kConsoleMs = 150'000;     // wake, connect and the first picture
constexpr qint64 kGoneMs = 15'000;         // the friend's peer gone this long: they left
constexpr int kRetryHoldMs = 90'000;       // a visit that ended before the picture keeps the console this long
constexpr int kHistory = 40;
const QString kRunKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kRunName = QStringLiteral("BooVI");

int64_t nowUs() { return sink::recordingClockUs(); }
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QString hex(const guest::TokenHash& h) { return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(h.data()), int(h.size())).toHex()); }
guest::TokenHash unhex(const QString& text) {
    guest::TokenHash h{};
    const QByteArray raw = QByteArray::fromHex(text.toLatin1());
    if (raw.size() == int(h.size())) std::copy(raw.begin(), raw.end(), h.begin());
    return h;
}

int minuteOf(const QString& hhmm) {
    const QTime t = QTime::fromString(hhmm, QStringLiteral("HH:mm"));
    return t.isValid() ? t.hour() * 60 + t.minute() : -1;
}
QString hhmm(int minute) { return minute < 0 ? QString() : QTime(minute / 60, minute % 60).toString(QStringLiteral("HH:mm")); }

QString modeName(guest::ControlMode m) {
    return m == guest::ControlMode::Together ? QStringLiteral("together") : m == guest::ControlMode::WatchOnly ? QStringLiteral("watch") : QStringLiteral("guest");
}

QString cleanName(QString name) {
    name = name.simplified();
    name.remove(QRegularExpression(QStringLiteral("[<>\"&]")));
    return name.left(24);
}

} // namespace

GuestAccessModel::GuestAccessModel(guest::GuestStreamer& streamer, GuestShareModel& share, QmlPlayerBridge& bridge, QObject* parent)
    : QObject(parent), streamer_(streamer), share_(share), bridge_(bridge), timer_(new QTimer(this)), restTimer_(new QTimer(this)) {
    load();
    publish();
    desk_.setHostName(hostName().toStdString());
    // A knock that begins a visit comes on an HTTP thread: the visit is driven here.
    desk_.onVisit = [this] { QMetaObject::invokeMethod(this, &GuestAccessModel::tick, Qt::QueuedConnection); };
    streamer_.setAccessDesk(&desk_);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &GuestAccessModel::tick);
    timer_->start();
    restTimer_->setSingleShot(true);
    connect(restTimer_, &QTimer::timeout, this, &GuestAccessModel::restConsole);
    connect(&bridge_, &QmlPlayerBridge::preferencesChanged, this, [this] { desk_.setHostName(hostName().toStdString()); });
    connect(&share_, &GuestShareModel::friendCut, this, &GuestAccessModel::cutFriend);
    testSetup();
    if (bridge_.preferences().value(QStringLiteral("gatewayEnabled")).toBool())
        QTimer::singleShot(1000, this, [this] { setGateway(true); });
}

GuestAccessModel::~GuestAccessModel() {
    desk_.onVisit = nullptr;
    streamer_.setAccessDesk(nullptr);
    share_.setHeld(false);
    share_.stop();   // waits for requests in flight, so none reaches the desk after this
    if (tray_) tray_->hide();
}

// ---- the store ----------------------------------------------------------------------------

void GuestAccessModel::load() {
    QFile file(QDir(bridge_.dataDirectory()).filePath(QStringLiteral("guest-access.dpapi")));
    if (!file.open(QIODevice::ReadOnly)) return;
    QByteArray plain = dpapiUnseal(file.readAll());
    const auto doc = QJsonDocument::fromJson(plain).object();
    plain.fill('\0');
    for (const auto& value : doc.value(QStringLiteral("accesses")).toArray()) {
        const auto o = value.toObject();
        guest::Access a;
        a.id = o.value(QStringLiteral("id")).toString().toStdString();
        a.name = o.value(QStringLiteral("name")).toString().toStdString();
        a.hash = unhex(o.value(QStringLiteral("hash")).toString());
        a.created = qint64(o.value(QStringLiteral("created")).toDouble());
        a.expires = qint64(o.value(QStringLiteral("expires")).toDouble());
        a.fromMinute = o.value(QStringLiteral("from")).toInt(-1);
        a.toMinute = o.value(QStringLiteral("to")).toInt(-1);
        a.ask = o.value(QStringLiteral("ask")).toBool(true);
        a.modes = uint8_t(o.value(QStringLiteral("modes")).toInt(guest::AllowGuest) & 7);
        a.feed = guest::parseFeed(o.value(QStringLiteral("feed")).toString().toStdString(), guest::Feed::Smooth120);
        if (!a.id.empty() && !a.name.empty()) accesses_.push_back(a);
    }
    for (const auto& value : doc.value(QStringLiteral("history")).toArray()) {
        const auto o = value.toObject();
        history_.push_back({o.value(QStringLiteral("name")).toString(), qint64(o.value(QStringLiteral("start")).toDouble()),
                            qint64(o.value(QStringLiteral("seconds")).toDouble()), o.value(QStringLiteral("outcome")).toString()});
    }
    veyra::log::info("guest-access", std::format("{} accesses loaded", accesses_.size()));
}

void GuestAccessModel::save() {
    QJsonArray list, events;
    for (const auto& a : accesses_)
        list.push_back(QJsonObject{{"id", QString::fromStdString(a.id)}, {"name", QString::fromStdString(a.name)}, {"hash", hex(a.hash)},
                                   {"created", double(a.created)}, {"expires", double(a.expires)}, {"from", a.fromMinute}, {"to", a.toMinute},
                                   {"ask", a.ask}, {"modes", int(a.modes)}, {"feed", QString::fromLatin1(guest::feedName(a.feed).data(), qsizetype(guest::feedName(a.feed).size()))}});
    for (const auto& e : history_)
        events.push_back(QJsonObject{{"name", e.name}, {"start", double(e.start)}, {"seconds", double(e.seconds)}, {"outcome", e.outcome}});
    const QByteArray sealed = dpapiSeal(QJsonDocument(QJsonObject{{"version", 1}, {"accesses", list}, {"history", events}}).toJson(QJsonDocument::Compact));
    QSaveFile file(QDir(bridge_.dataDirectory()).filePath(QStringLiteral("guest-access.dpapi")));
    if (sealed.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(sealed) != sealed.size() || !file.commit())
        veyra::log::warn("guest-access", "the accesses could not be saved");
}

// A revoked or regenerated access: the desk lets that friend go at once, and their connection is cut here.
void GuestAccessModel::publish() {
    for (const auto& g : desk_.setAccesses(accesses_)) leaveFriend(g, QStringLiteral("révoqué"));
    emit changed();
}

QString GuestAccessModel::makeToken(guest::Access& access) {
    std::string token = guest::newAccessToken();
    access.hash = guest::hashToken(token);
    const QString link = share_.accessLink(QString::fromStdString(token));
    SecureZeroMemory(token.data(), token.size());
    return link;
}

QString GuestAccessModel::hostName() const {
    return cleanName(bridge_.preferences().value(QStringLiteral("gatewayHostName")).toString());
}

// ---- the accesses -------------------------------------------------------------------------

QVariantList GuestAccessModel::accesses() const {
    QVariantList out;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const auto& a : accesses_)
        out.push_back(QVariantMap{{"id", QString::fromStdString(a.id)}, {"name", QString::fromStdString(a.name)},
                                  {"created", double(a.created) * 1000}, {"expires", double(a.expires) * 1000},
                                  {"expired", a.expires > 0 && now >= a.expires},
                                  {"from", hhmm(a.fromMinute)}, {"to", hhmm(a.toMinute)}, {"ask", a.ask}, {"modes", int(a.modes)},
                                  {"feed", QString::fromLatin1(guest::feedName(a.feed).data(), qsizetype(guest::feedName(a.feed).size()))}});
    return out;
}

QVariantList GuestAccessModel::history() const {
    QVariantList out;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it)
        out.push_back(QVariantMap{{"name", it->name}, {"start", double(it->start)}, {"seconds", double(it->seconds)}, {"outcome", it->outcome}});
    return out;
}

void GuestAccessModel::create(const QString& rawName, bool ask, int modes, const QString& feed, int days, const QString& from, const QString& to) {
    const QString name = cleanName(rawName);
    if (name.isEmpty()) { emit notice(tr("donne un nom à ton invité"), true); return; }
    if (std::ranges::any_of(accesses_, [&](const guest::Access& a) { return QString::fromStdString(a.name).compare(name, Qt::CaseInsensitive) == 0; })) {
        emit notice(tr("tu as déjà un invité nommé %1 : choisis un autre nom").arg(name), true);
        return;
    }
    guest::Access a;
    a.id = guest::randomBase64Url(9).substr(0, 12);
    a.name = name.toStdString();
    a.created = QDateTime::currentSecsSinceEpoch();
    a.expires = days > 0 ? a.created + qint64(days) * 86400 : 0;
    a.fromMinute = minuteOf(from); a.toMinute = minuteOf(to);
    if (a.fromMinute < 0 || a.toMinute < 0) a.fromMinute = a.toMinute = -1;
    a.ask = ask;
    a.modes = uint8_t((modes & 7) ? (modes & 7) : guest::AllowGuest);
    a.feed = guest::parseFeed(feed.toStdString(), guest::Feed::Smooth120);
    newLink_ = makeToken(a);
    newLinkName_ = name;
    accesses_.push_back(a);
    save();
    publish();
    veyra::log::info("guest-access", std::format("access created: {} ask={} modes={} feed={} days={} hours={}", a.name, a.ask, int(a.modes),
                                                 guest::feedName(a.feed), days, a.fromMinute >= 0 ? "set" : "any"));
    if (newLink_.isEmpty()) emit notice(tr("accès créé, mais ce pc n'a pas d'adresse durable : choisis ton domaine ou le tailnet dans « accès »."), true);
}

void GuestAccessModel::update(const QString& id, const QVariantMap& o) {
    for (auto& a : accesses_) {
        if (QString::fromStdString(a.id) != id) continue;
        if (o.contains(QStringLiteral("ask"))) a.ask = o.value(QStringLiteral("ask")).toBool();
        if (o.contains(QStringLiteral("modes"))) { const int m = o.value(QStringLiteral("modes")).toInt() & 7; if (m) a.modes = uint8_t(m); }
        if (o.contains(QStringLiteral("feed"))) a.feed = guest::parseFeed(o.value(QStringLiteral("feed")).toString().toStdString(), a.feed);
        if (o.contains(QStringLiteral("days"))) { const int d = o.value(QStringLiteral("days")).toInt(); a.expires = d > 0 ? QDateTime::currentSecsSinceEpoch() + qint64(d) * 86400 : 0; }
        if (o.contains(QStringLiteral("from")) || o.contains(QStringLiteral("to"))) {
            a.fromMinute = minuteOf(o.value(QStringLiteral("from")).toString()); a.toMinute = minuteOf(o.value(QStringLiteral("to")).toString());
            if (a.fromMinute < 0 || a.toMinute < 0) a.fromMinute = a.toMinute = -1;
        }
        save();
        publish();
        veyra::log::info("guest-access", std::format("access changed: {}", a.name));
        return;
    }
}

void GuestAccessModel::revoke(const QString& id) {
    const auto it = std::ranges::find_if(accesses_, [&](const guest::Access& a) { return QString::fromStdString(a.id) == id; });
    if (it == accesses_.end()) return;
    const std::string name = it->name;
    accesses_.erase(it);
    save();
    publish();   // the desk lets them go at once, their connection is cut; tick() ends the visit when it was theirs
    veyra::log::info("guest-access", "access revoked: " + name);
    tick();
}

void GuestAccessModel::regenerate(const QString& id) {
    for (auto& a : accesses_) {
        if (QString::fromStdString(a.id) != id) continue;
        newLink_ = makeToken(a);
        newLinkName_ = QString::fromStdString(a.name);
        save();
        publish();
        veyra::log::info("guest-access", "access regenerated: " + a.name);
        tick();
        return;
    }
}

void GuestAccessModel::copyNewLink() {
    if (auto* clipboard = QGuiApplication::clipboard(); clipboard && !newLink_.isEmpty()) {
        clipboard->setText(newLink_);
        emit notice(tr("lien copié · envoie-le à %1 en privé").arg(newLinkName_), false);
    }
}

void GuestAccessModel::dismissNewLink() {
    newLink_.fill(QLatin1Char('0'));
    newLink_.clear();
    newLinkName_.clear();
    emit changed();
}

void GuestAccessModel::clearHistory() { history_.clear(); save(); emit changed(); }

// ---- the gateway ---------------------------------------------------------------------------

void GuestAccessModel::setGateway(bool on) {
    if (on && !share_.active()) share_.start();
    if (on && !share_.active()) { emit notice(tr("l'accueil des invités n'a pas pu démarrer : %1").arg(share_.error()), true); on = false; }
    if (gateway_ == on) { updateTray(); return; }
    gateway_ = on;
    bridge_.setPreference(QStringLiteral("gatewayEnabled"), on);
    desk_.setOpen(on);
    share_.setHeld(on);
    if (!on) {
        if (current_) finish(QStringLiteral("accueil coupé"), "closed");
        if (share_.phase() != QLatin1String("connected")) share_.stop();
    }
    veyra::log::info("guest-access", std::format("gateway {}", on ? "on" : "off"));
    updateTray();
    emit changed();
}

bool GuestAccessModel::startWithWindows() const {
    return QSettings(kRunKey, QSettings::NativeFormat).contains(kRunName);
}

void GuestAccessModel::setStartWithWindows(bool on) {
    QSettings run(kRunKey, QSettings::NativeFormat);
    if (on) run.setValue(kRunName, QStringLiteral("\"%1\" --gateway-tray").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
    else run.remove(kRunName);
    run.sync();
    veyra::log::info("guest-access", std::format("start with windows {}", on ? "on" : "off"));
    emit changed();
}

void GuestAccessModel::attachWindow(QWindow* window, bool startHidden) {
    window_ = window;
    if (!window) return;
    connect(window, &QWindow::visibilityChanged, this, [this](QWindow::Visibility v) {
        if (v == QWindow::Minimized && gateway_ && tray_ && window_) QTimer::singleShot(0, window_, [w = window_] { if (w) w->hide(); });
    });
    if (startHidden) {
        if (!gateway_) setGateway(true);
        QTimer::singleShot(0, window, [w = window_] { if (w) w->hide(); });
    }
}

void GuestAccessModel::showWindow() {
    if (!window_) return;
    window_->showNormal();
    window_->raise();
    window_->requestActivate();
}

void GuestAccessModel::updateTray() {
    if (!gateway_) { if (tray_) tray_->hide(); return; }
    if (!tray_) {
        tray_ = new QSystemTrayIcon(this);
        QIcon icon = QGuiApplication::windowIcon();
        if (icon.isNull()) icon = QIcon(QCoreApplication::applicationDirPath() + QStringLiteral("/qml/Veyra/logo.png"));
        tray_->setIcon(icon);
        trayMenu_ = std::make_unique<QMenu>();
        tray_->setContextMenu(trayMenu_.get());
        connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) showWindow();
        });
        connect(tray_, &QSystemTrayIcon::messageClicked, this, &GuestAccessModel::showWindow);
    }
    tray_->setToolTip(tr("BooVI · accueil des invités actif"));
    trayMenu_->clear();
    trayMenu_->addAction(tr("BooVI · accueil des invités actif"))->setEnabled(false);
    trayMenu_->addSeparator();
    if (const auto g = pendingGuest()) {
        const QString name = QString::fromStdString(g->name);
        trayMenu_->addAction(tr("accepter %1").arg(name), this, &GuestAccessModel::approve);
        trayMenu_->addAction(tr("refuser %1").arg(name), this, &GuestAccessModel::refuse);
    } else if (current_) {
        trayMenu_->addAction(tr("couper la visite de %1").arg(QString::fromStdString(current_->name)), this, &GuestAccessModel::endVisit);
    }
    trayMenu_->addAction(tr("ouvrir BooVI"), this, &GuestAccessModel::showWindow);
    trayMenu_->addAction(tr("couper l'accueil"), this, [this] { setGateway(false); });
    trayMenu_->addAction(tr("quitter"), qApp, [] { if (auto* w = QGuiApplication::topLevelWindows().value(0)) w->close(); else QCoreApplication::quit(); });
    tray_->show();
}

// ---- visits --------------------------------------------------------------------------------

QVariantMap GuestAccessModel::visit() const {
    if (!current_) return {};
    QStringList friends;
    for (const auto& g : desk_.guests()) friends << QString::fromStdString(g.name);
    return {{"name", QString::fromStdString(current_->name)}, {"phase", QString::fromLatin1(guest::visitPhaseName(current_->phase).data())},
            {"busy", current_->consoleBusy}, {"mode", modeName(current_->mode)}, {"since", double(playingSinceMs_)}, {"friends", friends}};
}

// The friend waiting for the host's yes: the visit's starter while it asks, else a later friend.
std::optional<guest::VisitGuest> GuestAccessModel::pendingGuest() const {
    if (!current_) return std::nullopt;
    const bool asking = current_->phase == guest::VisitPhase::Asking;
    for (const auto& g : desk_.guests())
        if (asking ? g.accessId == current_->accessId : !g.approved) return g;
    return std::nullopt;
}

QVariantMap GuestAccessModel::pending() const {
    const auto g = pendingGuest();
    if (!g) return {};
    const qint64 asked = seen_.value(QString::fromStdString(g->accessId)).askedMs;
    const bool starter = current_->phase == guest::VisitPhase::Asking;
    return {{"name", QString::fromStdString(g->name)}, {"busy", starter && current_->consoleBusy}, {"joining", !starter},
            {"mode", g->watchOnly ? QStringLiteral("watch") : modeName(current_->mode)},
            {"left", int(std::max<qint64>(0, ((asked ? asked : askedAtMs_) + kAskMs - nowMs()) / 1000))}};
}

bool GuestAccessModel::sessionLive() const {
    if (!qEnvironmentVariable("VEYRA_TEST_ACCESS_SOURCE").isEmpty()) return bridge_.running();
    return bridge_.ps5().value(QStringLiteral("streaming")).toBool();
}

bool GuestAccessModel::consoleBusy() const {
    if (ownsSession_) return false;   // our own session, kept for the rest delay
    return bridge_.running() || bridge_.openingSource() || bridge_.ps5().value(QStringLiteral("active")).toBool();
}

void GuestAccessModel::startVisit(const guest::Visit& v) {
    current_ = v;
    askedAtMs_ = nowMs();
    consoleAtMs_ = playingSinceMs_ = 0;
    consoleStarted_ = applied_ = friendLeft_ = false;
    seen_.clear();
    restTimer_->stop();
    veyra::log::info("guest-access", std::format("{}: visit begins, {} console, mode {}, {}", v.name, v.consoleBusy ? "busy" : "free",
                                                 modeName(v.mode).toStdString(), v.phase == guest::VisitPhase::Asking ? "asking the host" : "approved by the access"));
    if (v.phase == guest::VisitPhase::Asking) ask(QString::fromStdString(v.name), v.consoleBusy, v.mode);
    updateTray();
}

void GuestAccessModel::ask(const QString& name, bool busy, guest::ControlMode mode) {
    const QString text = busy ? tr("%1 voudrait te rejoindre (%2)").arg(name, mode == guest::ControlMode::Together ? tr("à deux") : tr("regarder"))
                              : tr("%1 veut jouer sur ta ps5").arg(name);
    if (tray_) tray_->showMessage(QStringLiteral("BooVI"), text, QSystemTrayIcon::Information, int(kAskMs));
    emit notice(text, false);
    if (qEnvironmentVariableIntValue("VEYRA_TEST_ACCESS_APPROVE") == 1) QTimer::singleShot(1000, this, &GuestAccessModel::approve);
}

void GuestAccessModel::approve() {
    const auto g = pendingGuest();
    if (!g) return;
    // A click or key press already under way when the card appears must not let a guest in.
    const qint64 asked = seen_.value(QString::fromStdString(g->accessId)).askedMs;
    if (nowMs() - (asked ? asked : askedAtMs_) < 1200 && qEnvironmentVariableIntValue("VEYRA_TEST_ACCESS_APPROVE") != 1) {
        veyra::log::info("guest-access", g->name + ": approval ignored, the request is under 1.2 s old");
        return;
    }
    veyra::log::info("guest-access", g->name + ": approved by the host");
    if (current_->phase == guest::VisitPhase::Asking) desk_.setPhase(current_->consoleBusy ? guest::VisitPhase::Ready : guest::VisitPhase::Waking);
    else desk_.approveGuest(g->accessId);
    tick();
}

void GuestAccessModel::refuse() {
    const auto g = pendingGuest();
    if (!g) return;
    if (current_->phase == guest::VisitPhase::Asking) { finish(QStringLiteral("refusé"), "refused"); return; }
    leaveFriend(*g, QStringLiteral("refusé"), "refused");
}

void GuestAccessModel::endVisit() {
    if (!current_) return;
    finish(QStringLiteral("coupé"), "ended");
}

void GuestAccessModel::startConsole() {
    consoleStarted_ = true;
    consoleAtMs_ = nowMs();
    if (sessionLive()) { veyra::log::info("guest-access", "console session already live"); return; }
    ownsSession_ = true;
    wokeByAccess_ = false;
    if (const QString video = qEnvironmentVariable("VEYRA_TEST_ACCESS_SOURCE"); !video.isEmpty()) {
        veyra::log::info("guest-access", "test: a local video stands in for the PS5");
        bridge_.openPath(video);
        return;
    }
    if (bridge_.ps5().value(QStringLiteral("profile")).toString().isEmpty()) bridge_.ps5Load();
    veyra::log::info("guest-access", "PS5 Play: probe, wake when resting, connect");
    bridge_.ps5Play();
}

// One friend of the visit leaves it; the visit goes on with the others.
void GuestAccessModel::leaveFriend(const guest::VisitGuest& g, const QString& outcome, const char* state, const QString& detail) {
    const QString id = QString::fromStdString(g.accessId);
    const Seen s = seen_.take(id);
    desk_.leave(g.accessId, nowUs(), state ? state : "", detail.toStdString());
    share_.dropAccess(QString::fromStdString(g.accessId));
    const qint64 seconds = s.firstMs ? (s.lastMs - s.firstMs) / 1000 : 0;
    history_.push_back({QString::fromStdString(g.name), s.joinedMs ? s.joinedMs : nowMs(), seconds, outcome});
    friendLeft_ = true;
    if (history_.size() > size_t(kHistory)) history_.erase(history_.begin(), history_.end() - kHistory);
    save();
    veyra::log::info("guest-access", std::format("{}: leaves the visit ({}) after {} s", g.name, outcome.toStdString(), seconds));
    updateTray();
    emit changed();
}

// The host disconnected this friend from the share panel: their visit ends, and their page reads
// why instead of knocking straight back in.
void GuestAccessModel::cutFriend(const QString& accessId) {
    const auto guests = desk_.guests();
    const auto it = std::ranges::find_if(guests, [&](const guest::VisitGuest& g) { return QString::fromStdString(g.accessId) == accessId; });
    if (it == guests.end()) return;
    leaveFriend(*it, QStringLiteral("déconnecté"), "ended", tr("%1 t'a déconnecté").arg(hostName().isEmpty() ? tr("ton ami") : hostName()));
    if (desk_.guests().empty()) finish(QStringLiteral("déconnecté"));
}

void GuestAccessModel::tick() {
    desk_.setConsoleBusy(consoleBusy());
    desk_.setCapacity(std::clamp(bridge_.preferences().value(QStringLiteral("guestMaxViewers"), 4).toInt(), 1, 6));
    const auto v = desk_.visit();
    // The desk dropped the visit (a revoked or regenerated access).
    if (current_ && (!v || v->startedUs != current_->startedUs)) finish(QStringLiteral("révoqué"));
    if (!v) {
        // Our kept session went away on its own (the host took the console, or it failed).
        if (ownsSession_ && !restTimer_->isActive() && !bridge_.running() && !bridge_.openingSource() && !bridge_.ps5().value(QStringLiteral("active")).toBool()) ownsSession_ = false;
        return;
    }
    if (!current_) startVisit(*v);
    const guest::VisitPhase phaseBefore = current_->phase;
    current_->phase = v->phase;
    const qint64 now = nowMs();
    const auto ps5 = bridge_.ps5();
    const QStringList here = share_.accessIds();
    // Every friend in the visit: asked, here, or gone.
    for (const auto& g : desk_.guests()) {
        const QString id = QString::fromStdString(g.accessId), name = QString::fromStdString(g.name);
        Seen& s = seen_[id];
        if (!s.joinedMs) {
            s.joinedMs = now;
            if (!g.approved && id != QString::fromStdString(v->accessId)) { s.askedMs = now; ask(name, false, g.watchOnly ? guest::ControlMode::WatchOnly : v->mode); }
        }
        if (here.contains(id)) {
            if (!s.firstMs) {
                s.firstMs = now;
                veyra::log::info("guest-access", g.name + ": connected");
                emit notice(tr("%1 a rejoint ta ps5").arg(name), false);
            }
            s.lastMs = now;
            continue;
        }
        const bool starter = id == QString::fromStdString(v->accessId);
        const bool knocking = nowUs() - g.knockUs < kAbandonUs;
        if (s.firstMs && now - s.lastMs > kGoneMs) { if (!starter || desk_.guests().size() > 1) leaveFriend(g, QStringLiteral("joué")); continue; }
        if (!starter && !g.approved && s.askedMs && now - s.askedMs > kAskMs) { leaveFriend(g, QStringLiteral("sans réponse"), "refused", tr("pas de réponse de %1").arg(hostName().isEmpty() ? tr("ton ami") : hostName())); continue; }
        if (!starter && !s.firstMs && !knocking && now - s.joinedMs > kAbandonUs / 1000) { leaveFriend(g, QStringLiteral("parti avant l'image")); continue; }
    }
    // The starter left while others stay: the visit is theirs now; it ends with the last one.
    const auto guests = desk_.guests();
    auto present = [&](const guest::VisitGuest& g) { return here.contains(QString::fromStdString(g.accessId)); };
    const bool starterHere = std::any_of(guests.begin(), guests.end(), [&](const guest::VisitGuest& g) { return g.accessId == v->accessId && present(g); });
    const int64_t silentUs = nowUs() - v->knockUs;
    switch (v->phase) {
    case guest::VisitPhase::Asking:
        if (silentUs > kAbandonUs) { finish(QStringLiteral("parti avant l'accord")); return; }
        if (now - askedAtMs_ > kAskMs) {
            finish(QStringLiteral("sans réponse"), "refused", tr("pas de réponse de %1").arg(hostName().isEmpty() ? tr("ton ami") : hostName()));
            return;
        }
        break;
    case guest::VisitPhase::Waking:
    case guest::VisitPhase::Connecting: {
        // The friends connect while the console wakes (their pages stop knocking once they are in).
        if (!applied_) { share_.applyVisit(current_->mode, current_->feed); applied_ = true; }
        if (silentUs > kAbandonUs && !starterHere) { finish(QStringLiteral("parti pendant le réveil")); return; }
        if (!consoleStarted_) startConsole();
        if (sessionLive()) {
            wokeByAccess_ = wokeByAccess_ || (ownsSession_ && ps5.value(QStringLiteral("woke")).toBool());
            desk_.setPhase(guest::VisitPhase::Ready);
            veyra::log::info("guest-access", std::format("{}: console streaming after {:.1f} s{}{}", current_->name, double(now - consoleAtMs_) / 1000.0,
                                                         wokeByAccess_ ? ", woken by this visit" : "", starterHere ? ", the friend already connected" : ""));
            break;
        }
        if (v->phase == guest::VisitPhase::Waking && (ps5.value(QStringLiteral("active")).toBool() || bridge_.openingSource())) desk_.setPhase(guest::VisitPhase::Connecting);
        if (ps5.value(QStringLiteral("pinRequired")).toBool() && !bridge_.preferences().value(QStringLiteral("ps5RememberPin")).toBool() && now - consoleAtMs_ > 5000) {
            finish(QStringLiteral("code ps5 non mémorisé"), "failed", tr("la ps5 demande son code de connexion, que ce pc n'a pas mémorisé"));
            return;
        }
        const bool idle = !ps5.value(QStringLiteral("busy")).toBool() && !ps5.value(QStringLiteral("active")).toBool() && !bridge_.openingSource() && !bridge_.running();
        if ((idle && now - consoleAtMs_ > 8000) || bridge_.failed() || now - consoleAtMs_ > kConsoleMs) {
            finish(QStringLiteral("console injoignable"), "failed", tr("la ps5 ne répond pas"));
            return;
        }
        break;
    }
    case guest::VisitPhase::Ready:
    case guest::VisitPhase::Playing: {
        if (!applied_) { share_.applyVisit(current_->mode, current_->feed); applied_ = true; }
        const bool anyone = std::any_of(guests.begin(), guests.end(), present);
        if (anyone && v->phase == guest::VisitPhase::Ready) {
            desk_.setPhase(guest::VisitPhase::Playing);
            playingSinceMs_ = now;
            veyra::log::info("guest-access", std::format("{}: playing, {:.1f} s after the visit began", current_->name, double(now - askedAtMs_) / 1000.0));
        }
        if (guests.empty()) { finish(QStringLiteral("joué")); return; }
        if (!anyone && v->phase == guest::VisitPhase::Ready && silentUs > kAbandonUs) { finish(QStringLiteral("parti avant l'image")); return; }
        if (!anyone && v->phase == guest::VisitPhase::Playing) {
            bool gone = true;
            for (const auto& g : guests) { const Seen s = seen_.value(QString::fromStdString(g.accessId)); gone &= !s.lastMs || now - s.lastMs > kGoneMs; }
            if (gone) { finish(QStringLiteral("joué")); return; }
        }
        // Nobody's pad moved for the idle delay: the visit ends and the console rests.
        const int idleMinutes = bridge_.preferences().value(QStringLiteral("gatewayIdleMinutes"), 20).toInt();
        if (anyone && idleMinutes > 0 && share_.playIdleSeconds() > idleMinutes * 60.0) { finish(QStringLiteral("inactif"), "ended", tr("plus de manette depuis %1 min").arg(idleMinutes)); return; }
        // The kept or joined stream ended under the friends.
        if (!current_->consoleBusy && !sessionLive() && !bridge_.openingSource() && now - consoleAtMs_ > 10000) {
            finish(QStringLiteral("console perdue"), "failed", tr("la ps5 a coupé le stream"));
            return;
        }
        break;
    }
    }
    if (const auto after = desk_.visit(); current_ && after && after->phase != phaseBefore) { current_->phase = after->phase; updateTray(); }
    emit changed();
}

void GuestAccessModel::finish(const QString& outcome, const char* state, const QString& detail) {
    if (!current_) return;
    const guest::Visit v = *current_;
    current_.reset();
    const auto guests = desk_.guests();
    desk_.end(nowUs(), state ? state : "", detail.toStdString());
    const qint64 now = nowMs();
    for (const auto& g : guests) {
        share_.dropAccess(QString::fromStdString(g.accessId));
        const Seen s = seen_.value(QString::fromStdString(g.accessId));
        // Play time ends when the friend was last seen, not after the grace that tells they left.
        const qint64 seconds = s.firstMs ? ((s.lastMs ? s.lastMs : now) - s.firstMs) / 1000 : 0;
        history_.push_back({QString::fromStdString(g.name), s.joinedMs ? s.joinedMs : askedAtMs_, seconds, outcome});
        veyra::log::info("guest-access", std::format("{}: visit ended ({}) after {} s connected", g.name, outcome.toStdString(), seconds));
    }
    if (guests.empty() && !friendLeft_) history_.push_back({QString::fromStdString(v.name), askedAtMs_, 0, outcome});
    seen_.clear();
    if (applied_) share_.endVisit();
    if (history_.size() > size_t(kHistory)) history_.erase(history_.begin(), history_.end() - kHistory);
    save();
    if (outcome != QStringLiteral("refusé") && playingSinceMs_) emit notice(tr("la visite de %1 est finie").arg(QString::fromStdString(v.name)), false);
    // The console: only a session this gateway started is stopped or put to rest. A visit that
    // ended before the picture keeps it up a while: the friend's retry finds it streaming.
    if (ownsSession_ && !v.consoleBusy) {
        // Friends now connect while the console wakes: one who leaves within 30 s of the picture
        // (or before it) is as likely to come right back.
        const bool shortPlay = outcome == QStringLiteral("joué") && (!playingSinceMs_ || now - playingSinceMs_ < 30'000);
        const bool early = shortPlay || outcome == QLatin1String("parti avant l'image") || outcome == QStringLiteral("parti pendant le réveil");
        const int rest = bridge_.preferences().value(QStringLiteral("gatewayRestMinutes"), 10).toInt();
        if (early) {
            restTimer_->start(kRetryHoldMs);
            veyra::log::info("guest-access", std::format("console kept {} s for a retry", kRetryHoldMs / 1000));
        } else if (wokeByAccess_ && rest > 0) {
            restTimer_->start(rest * 60'000);
            veyra::log::info("guest-access", std::format("console kept {} min, then rest mode", rest));
        } else {
            veyra::log::info("guest-access", "console session stopped");
            bridge_.stopPlayback();
            ownsSession_ = false;
        }
    }
    playingSinceMs_ = 0;
    updateTray();
    emit changed();
}

void GuestAccessModel::restConsole() {
    if (!ownsSession_ || current_) return;
    ownsSession_ = false;
    // The host is at the PC: the session is theirs now.
    if (window_ && window_->isActive()) { veyra::log::info("guest-access", "rest skipped: the host is at the PC"); return; }
    if (wokeByAccess_ && bridge_.ps5GotoBed()) QTimer::singleShot(1500, &bridge_, [b = &bridge_] { b->stopPlayback(); });
    else bridge_.stopPlayback();
    wokeByAccess_ = false;
}

// ---- tests ----------------------------------------------------------------------------------

void GuestAccessModel::testSetup() {
    const QString file = qEnvironmentVariable("VEYRA_TEST_ACCESS_FILE");
    if (file.isEmpty()) return;
    // After the share session knows its port: the links name it.
    QTimer::singleShot(2500, this, [this, file] {
        // A name is one access: the last run's "test" goes first.
        for (const auto& a : std::vector(accesses_)) if (a.name == "test") revoke(QString::fromStdString(a.id));
        create(QStringLiteral("test"), qEnvironmentVariableIsSet("VEYRA_TEST_ACCESS_APPROVE"), guest::AllowGuest, QStringLiteral("real"), 0, {}, {});
        const QString link = newLink_;
        create(QStringLiteral("révoqué"), false, guest::AllowGuest, QStringLiteral("real"), 0, {}, {});
        const QString revoked = newLink_;
        revoke(QString::fromStdString(accesses_.back().id));
        dismissNewLink();
        QFile out(file);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write((link + QLatin1Char('\n') + revoked + QLatin1Char('\n')).toUtf8());
    });
    // VEYRA_TEST_ACCESS_REVOKE_AT=<s>: the "test" access is revoked that long after start, its
    // friend possibly connected.
    if (const int at = qEnvironmentVariableIntValue("VEYRA_TEST_ACCESS_REVOKE_AT"); at > 0)
        QTimer::singleShot(at * 1000, this, [this] {
            for (const auto& a : accesses_)
                if (a.name == "test") { veyra::log::info("guest-access", "test: the access is revoked"); revoke(QString::fromStdString(a.id)); return; }
        });
}

} // namespace veyra::ui
