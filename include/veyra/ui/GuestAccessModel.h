#pragma once
// Fork, invités: friends with a personal, revocable access ("accès permanent") reach this PC,
// which wakes the PS5, answers its login PIN and lets them play, while the host is away. The
// PC is the gateway: the pairing keys and the PIN never leave it. Exposed to QML as `vyAccess`.
//
// - The accesses and the visit history live in <data dir>/guest-access.dpapi, DPAPI-encrypted
//   for this Windows user; each token only as its SHA-256 (GuestAccess.h). A new or regenerated
//   link is shown once.
// - "accepter les invités en mon absence" (gatewayEnabled) keeps the share session and its
//   tunnel open with no stream running (GuestShareModel::setHeld), shows a tray icon and
//   minimizes to it. "démarrer avec Windows" writes HKCU\...\Run only when ticked.
// - A visit: the friend's page knocks (AccessDesk); the host is asked when the access says so
//   (in-app prompt, tray notification, 60 s then refused); a free console is woken and
//   connected with the existing PS5 Play flow (remembered login PIN, PS tap after a wake). Once
//   the host said yes the friend's page connects while the console wakes; the picture follows.
//   Other friends join the running visit (the room decides who holds the controller). When the
//   last one leaves, or nobody's pad moves for the idle delay, the visit ends: the PS5 session
//   stops, or, when this visit woke the console, it is put to rest after the rest delay; a visit
//   that ended before the picture or within 30 s of it keeps it 90 s for a retry.
//
// Tests: VEYRA_TEST_ACCESS_SOURCE=<video> plays that file instead of the PS5 Play flow;
// VEYRA_TEST_ACCESS_FILE=<file> creates an access "test" (and a revoked one) at start and writes
// their links there; VEYRA_TEST_ACCESS_APPROVE set makes "test" ask the host, =1 approves it after a second.
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <optional>

#include "veyra/guest/GuestAccess.h"

class QSystemTrayIcon;
class QMenu;
class QTimer;
class QWindow;

namespace veyra::guest { class GuestStreamer; }

namespace veyra::ui {

class QmlPlayerBridge;
class GuestShareModel;

class GuestAccessModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList accesses READ accesses NOTIFY changed)
    Q_PROPERTY(QVariantList history READ history NOTIFY changed)
    Q_PROPERTY(bool gateway READ gateway NOTIFY changed)
    Q_PROPERTY(bool startWithWindows READ startWithWindows NOTIFY changed)
    Q_PROPERTY(QVariantMap visit READ visit NOTIFY changed)
    Q_PROPERTY(QVariantMap pending READ pending NOTIFY changed)
    Q_PROPERTY(QString newLink READ newLink NOTIFY changed)
    Q_PROPERTY(QString newLinkName READ newLinkName NOTIFY changed)
public:
    GuestAccessModel(guest::GuestStreamer& streamer, GuestShareModel& share, QmlPlayerBridge& bridge, QObject* parent = nullptr);
    ~GuestAccessModel() override;

    QVariantList accesses() const;
    QVariantList history() const;
    bool gateway() const { return gateway_; }
    bool startWithWindows() const;
    QVariantMap visit() const;
    QVariantMap pending() const;
    QString newLink() const { return newLink_; }
    QString newLinkName() const { return newLinkName_; }

    // modes: GuestAccess.h AllowedMode bits; feed "real", "60", "120"; days 0: no end;
    // from/to "HH:MM", both empty: any time.
    Q_INVOKABLE void create(const QString& name, bool ask, int modes, const QString& feed, int days, const QString& from, const QString& to);
    Q_INVOKABLE void update(const QString& id, const QVariantMap& options);
    Q_INVOKABLE void revoke(const QString& id);
    Q_INVOKABLE void regenerate(const QString& id);
    Q_INVOKABLE void copyNewLink();
    Q_INVOKABLE void dismissNewLink();
    Q_INVOKABLE void approve();
    Q_INVOKABLE void refuse();
    Q_INVOKABLE void endVisit();                 // "couper": the host ends the running visit
    Q_INVOKABLE void setGateway(bool on);
    Q_INVOKABLE void setStartWithWindows(bool on);
    Q_INVOKABLE void clearHistory();

    // main.cpp: the window that hides to the tray; startHidden for --gateway-tray.
    void attachWindow(QWindow* window, bool startHidden);

signals:
    void changed();
    void notice(const QString& text, bool warning);

private:
    struct Event { QString name; qint64 start = 0, seconds = 0; QString outcome; };
    void load();
    void save();
    void publish();                          // the accesses to the desk
    void tick();
    void startVisit(const guest::Visit& v);
    void ask(const QString& name, bool busy, guest::ControlMode mode);
    std::optional<guest::VisitGuest> pendingGuest() const;
    void leaveFriend(const guest::VisitGuest& g, const QString& outcome, const char* state = nullptr, const QString& detail = {});
    void cutFriend(const QString& accessId);
    void startConsole();
    bool sessionLive() const;
    bool consoleBusy() const;
    void finish(const QString& outcome, const char* state = nullptr, const QString& detail = {});
    void restConsole();
    void showWindow();
    void updateTray();
    QString hostName() const;
    QString makeToken(guest::Access& access);   // a fresh token; only its hash stays on the access
    void testSetup();

    guest::GuestStreamer& streamer_;
    GuestShareModel& share_;
    QmlPlayerBridge& bridge_;
    guest::AccessDesk desk_;
    std::vector<guest::Access> accesses_;
    std::vector<Event> history_;
    QTimer* timer_ = nullptr;
    QTimer* restTimer_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    std::unique_ptr<QMenu> trayMenu_;
    QPointer<QWindow> window_;
    bool gateway_ = false;
    QString newLink_, newLinkName_;

    // The visit being driven, a copy of the desk's.
    std::optional<guest::Visit> current_;
    qint64 askedAtMs_ = 0, consoleAtMs_ = 0, playingSinceMs_ = 0;
    // Each friend of the visit, by access id: when they joined it, were asked about, first and
    // last seen connected.
    struct Seen { qint64 joinedMs = 0, askedMs = 0, firstMs = 0, lastMs = 0; };
    QHash<QString, Seen> seen_;
    bool consoleStarted_ = false, applied_ = false;
    bool friendLeft_ = false;   // a friend of this visit already has their line in the history
    bool ownsSession_ = false;   // this gateway started the console's session (kept for the rest delay)
    bool wokeByAccess_ = false;  // that session woke the console from rest mode
};

} // namespace veyra::ui
