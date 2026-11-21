#pragma once
// Fork, partage: the stream toolbar's "partager" panel and the settings section talk to the
// remote guests (GuestStreamer.h) through this object, exposed to QML as `vyGuest`.
//
// Reachability (Réglages → partage → accès, `guestReach`): "broker" (default when the build has
// one) is this PC's room on BooVI's broker, https://<brokerHost>/<room id>#t=<token>, opened by
// GuestStreamer with the share session; "tailnet" goes through Tailscale, where reading its
// state happens on its own and `tailscale serve` (an https link inside the tailnet) runs only
// when the host clicks the button, undone when sharing stops. The token rides in the link's
// fragment, which no browser sends to a server.
//
// Several friends on one share (RoomModel.h): `viewers` lists them, `room` says who holds the
// controller, who waits and the last dice roll; the host passes the controller, sets who may
// play, kicks one viewer or renews the link. The room's events become the play history
// (<data dir>/guest-plays.dpapi) and the Discord notices (DiscordNotifier.h). While anyone
// watches the PC does not go to sleep. A share stuck on a request past its deadline, with
// nobody connected, restarts on its own.
//
// The friends' gateway (GuestAccessModel.h) holds the share session open while it welcomes
// guests: "arrêter le partage" then only renews the share link.
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <map>
#include <vector>

#include "veyra/guest/GuestQuality.h"
#include "veyra/guest/GuestProtocol.h"
#include "veyra/guest/RoomModel.h"

class QProcess;
class QTimer;

namespace veyra::guest { class GuestStreamer; struct ShareSettings; }

namespace veyra::ui {

class QmlPlayerBridge;
class DiscordNotifier;

class GuestShareModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(QString link READ link NOTIFY changed)
    Q_PROPERTY(QString localLink READ localLink NOTIFY changed)
    Q_PROPERTY(QString reach READ reach NOTIFY changed)
    Q_PROPERTY(bool tailscaleUp READ tailscaleUp NOTIFY changed)
    Q_PROPERTY(bool httpsReady READ httpsReady NOTIFY changed)
    Q_PROPERTY(QString broker READ broker NOTIFY changed)
    Q_PROPERTY(QString brokerHost READ brokerHost CONSTANT)
    Q_PROPERTY(QString relay READ relay NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList viewers READ viewers NOTIFY changed)
    Q_PROPERTY(QVariantMap room READ room NOTIFY changed)
    Q_PROPERTY(QVariantList plays READ plays NOTIFY changed)
    Q_PROPERTY(QVariantMap discord READ discord NOTIFY changed)
    Q_PROPERTY(double sentMbps READ sentMbps NOTIFY changed)
    Q_PROPERTY(bool held READ held NOTIFY changed)
public:
    GuestShareModel(guest::GuestStreamer& guest, QmlPlayerBridge& bridge, QObject* parent = nullptr);
    ~GuestShareModel() override;

    bool active() const { return active_; }
    QString phase() const { return phase_; }
    QString link() const;
    QString localLink() const;
    QString reach() const;
    bool tailscaleUp() const { return !tailscaleIp_.isEmpty(); }
    bool httpsReady() const { return httpsReady_; }
    QString broker() const { return broker_; }   // the room: "off", "connecting", "connected"
    QString brokerHost() const;                  // "" when this build has no broker
    QString relay() const { return relay_; }     // when the direct path fails: "owner", "key", "none"
    bool busy() const { return busy_; }
    QString mode() const;
    QString error() const { return error_; }
    // Each viewer: tag, name, access, client, seat, holder, queue, connected, path, and the stats.
    QVariantList viewers() const { return viewers_; }
    // holder (tag), holderName, held (s), owner, queue [names], turn (min), left (s), dice {…}, max, diceAllowed.
    QVariantMap room() const { return room_; }
    QVariantList plays() const;
    QVariantMap discord() const;
    double sentMbps() const { return sentMbps_; }
    bool held() const { return held_; }
    // The accesses of the friends connected now.
    QStringList accessIds() const;
    double playIdleSeconds() const { return playIdleSeconds_; }

    // The gateway: keep the share session open (stop() then only renews the link), and a visit's
    // mode and feed for its length, without touching the host's own preferences.
    void setHeld(bool held);
    void applyVisit(guest::ControlMode mode, guest::Feed feedCap);
    void endVisit();
    void dropAccess(const QString& accessId);
    // https://<broker>/<this PC's room>#a=<token>, or this PC's tailnet address.
    QString accessLink(const QString& token) const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void kick(const QString& tag);
    Q_INVOKABLE void renewLink();
    Q_INVOKABLE void setMode(const QString& mode);
    Q_INVOKABLE void copyLink();
    Q_INVOKABLE void give(const QString& tag);
    Q_INVOKABLE void takeBack();
    Q_INVOKABLE void handBack();
    Q_INVOKABLE void setSeat(const QString& tag, const QString& seat);
    Q_INVOKABLE void roll();
    Q_INVOKABLE bool setDiscordWebhook(const QString& url);
    Q_INVOKABLE void clearDiscordWebhook();
    Q_INVOKABLE void testDiscord();
    Q_INVOKABLE void setDiscordEvent(const QString& name, bool on);
    Q_INVOKABLE void clearPlays();
    // Tailscale, on the host's click only: an https link inside the tailnet.
    Q_INVOKABLE void enableHttps();
    Q_INVOKABLE void disableHttps();

signals:
    void changed();
    void notice(const QString& text, bool warning);
    // The host disconnected a friend who came with an access: their visit ends (GuestAccessModel).
    void friendCut(const QString& accessId);

private:
    struct Play { QString name; qint64 at = 0, seconds = 0; };
    void poll();
    void queryTailscale();
    void runTailscale(const QStringList& args, const QString& what);
    void applyPreferences();
    QString reachPreference() const;
    QString pathLabel(const QString& remote, bool relayed) const;
    bool startStreamer();
    void drainRoom();
    void keepAwake(bool on);
    void watchdog(bool stuck, bool controlAlive, bool anyone);
    void loadPlays();
    void savePlays();
    void pushHistory();
    void testScript();

    guest::GuestStreamer& guest_;
    QmlPlayerBridge& bridge_;
    DiscordNotifier* discord_ = nullptr;
    QTimer* timer_ = nullptr;
    bool active_ = false, httpsReady_ = false, busy_ = false;
    bool weServed_ = false;       // what this session turned on, to undo
    bool held_ = false, visiting_ = false, awake_ = false, stuckSaid_ = false;
    qint64 restartedAtMs_ = 0;
    QString phase_ = QStringLiteral("off"), error_, token_, tailscaleIp_, dnsName_;
    QString shareReach_ = QStringLiteral("tailnet"), broker_ = QStringLiteral("off"), relay_, publicUrl_, linkWritten_;
    int port_ = 0, ticks_ = 0;
    QVariantList viewers_;
    QVariantMap room_;
    double sentMbps_ = 0, playIdleSeconds_ = 0;
    std::vector<Play> plays_;
    QVariantMap peers_;           // tailscale IP -> "direct" or the DERP region it relays through
};

} // namespace veyra::ui
