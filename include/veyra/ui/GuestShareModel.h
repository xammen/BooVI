#pragma once
// Fork, partage: the stream toolbar's "partager" panel and the settings section talk to the
// remote guest (GuestStreamer.h) through this object, exposed to QML as `vyGuest`.
//
// Reachability (Réglages → partage → accès, `guestReach`): "tunnel" is the user's named
// Cloudflare tunnel (https://<cloudflareHost>), "quick" an account-less trycloudflare.com
// link, both opened by GuestStreamer with the share session; "tailnet" goes through
// Tailscale, where reading its state happens on its own and `tailscale serve` (an https link
// inside the tailnet) runs only when the host clicks the button, undone when sharing stops.
//
// The friends' gateway (GuestAccessModel.h) holds the share session open while it welcomes
// guests: "arrêter le partage" then only disconnects the guest and changes the share link.
#include <QObject>
#include <QString>
#include <QVariantMap>

#include "veyra/guest/GuestQuality.h"
#include "veyra/guest/GuestProtocol.h"

class QProcess;
class QTimer;

namespace veyra::guest { class GuestStreamer; }

namespace veyra::ui {

class QmlPlayerBridge;

class GuestShareModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(QString link READ link NOTIFY changed)
    Q_PROPERTY(QString localLink READ localLink NOTIFY changed)
    Q_PROPERTY(QString reach READ reach NOTIFY changed)
    Q_PROPERTY(bool tailscaleUp READ tailscaleUp NOTIFY changed)
    Q_PROPERTY(bool httpsReady READ httpsReady NOTIFY changed)
    Q_PROPERTY(QString tunnel READ tunnel NOTIFY changed)
    Q_PROPERTY(QString cloudflareHost READ cloudflareHost NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantMap stats READ stats NOTIFY changed)
    Q_PROPERTY(bool held READ held NOTIFY changed)
    Q_PROPERTY(QString accessName READ accessName NOTIFY changed)
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
    QString tunnel() const { return tunnel_; }   // "off", "starting", "connected", "failed"
    QString cloudflareHost() const { return cloudflareHost_; }
    bool busy() const { return busy_; }
    QString mode() const;
    QString error() const { return error_; }
    QVariantMap stats() const { return stats_; }
    bool held() const { return held_; }
    QString accessName() const { return accessName_; }   // the connected friend's access, "" for the share link

    // The gateway: keep the share session open (stop() then only kicks), and a visit's mode and
    // feed for its length, without touching the host's own preferences.
    void setHeld(bool held);
    void applyVisit(guest::ControlMode mode, guest::Feed feedCap);
    void endVisit();
    // https://<the named tunnel>/?a=<token>, or this PC's tailnet address; empty when no address
    // outlives a share session (a quick tunnel's URL changes each time).
    QString accessLink(const QString& token) const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void kick();
    Q_INVOKABLE void setMode(const QString& mode);
    Q_INVOKABLE void copyLink();
    // Tailscale, on the host's click only: an https link inside the tailnet.
    Q_INVOKABLE void enableHttps();
    Q_INVOKABLE void disableHttps();

signals:
    void changed();
    void notice(const QString& text, bool warning);

private:
    void poll();
    void queryTailscale();
    void runTailscale(const QStringList& args, const QString& what);
    void applyPreferences();
    QString reachPreference() const;
    QString pathLabel(const QString& remote, bool relayed) const;

    guest::GuestStreamer& guest_;
    QmlPlayerBridge& bridge_;
    QTimer* timer_ = nullptr;
    bool active_ = false, httpsReady_ = false, busy_ = false;
    bool weServed_ = false;       // what this session turned on, to undo
    bool held_ = false, visiting_ = false;
    QString accessName_;
    QString phase_ = QStringLiteral("off"), error_, token_, tailscaleIp_, dnsName_;
    QString shareReach_ = QStringLiteral("tailnet"), tunnel_ = QStringLiteral("off"), publicUrl_, cloudflareHost_, linkWritten_;
    int port_ = 0, ticks_ = 0;
    QVariantMap stats_;
    QVariantMap peers_;           // tailscale IP -> "direct" or the DERP region it relays through
};

} // namespace veyra::ui
