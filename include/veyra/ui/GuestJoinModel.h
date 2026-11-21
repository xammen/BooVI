#pragma once
// Fork, "rejoindre un partage": the home row and the connect page's "partage" flow talk to
// the native guest client (GuestClientSource.h) through this object (`veyra.join` in QML).
// A pasted share link (?t=) is kept in memory for this run only: its token is a secret of the
// host's share session, never written to the preferences or the log. A friend's access link
// (?a=, long-lived) is saved once it has played, DPAPI-encrypted for this Windows user, as
// "ps5 de <host>", so the home row joins it again in one click; forgetSaved() drops it.
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <functional>

#include "veyra/source/GuestClientSource.h"

namespace veyra::ui {

class GuestJoinModel final : public QObject {
    Q_OBJECT
    // {phase: "idle"|"connecting"|"streaming"|"ended"|"failed", streaming, linkValid, host,
    //  lastLabel, message, access, saved ("ps5 de <host>", "" without one), browser (offer the browser),
    //  stream{receivedFps, videoMbps, dropped, rttMs, decodeMs, codec, decoder, gpu,
    //  hostToScreenMs, lossPercent, relayed, mode, extendedInput},
    //  room{you, members[{id,name,seat,native}], holder, owner, held, queue, turn, left, dice, allowDice, max, role}}
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
public:
    // Starts the engine on the link; false when it cannot (no video window).
    using LaunchFn = std::function<bool(const QString& link, const QString& host)>;

    explicit GuestJoinModel(QObject* parent = nullptr);
    void setLaunchHandler(LaunchFn handler) { launch_ = std::move(handler); }
    void setRoomHandler(std::function<void(const std::string&)> handler) { roomHandler_ = std::move(handler); }
    void setStoreFile(const QString& path);   // the saved access; loads it
    void updateStream(bool active, const source::GuestClientStats& stats);
    QVariantMap state() const;

    Q_INVOKABLE bool setLink(const QString& link);   // true when it reads as a share link
    Q_INVOKABLE bool pasteLink();                    // the clipboard's text, when it is one
    Q_INVOKABLE bool join();
    Q_INVOKABLE bool resumeLast() { return linkValid_ && join(); }
    Q_INVOKABLE void forgetSaved();
    // The same link in the default browser, when the join found no network path.
    Q_INVOKABLE bool openInBrowser();
    // The room of the joined share (state.room): "ask", "cancel", "release", "roll".
    Q_INVOKABLE void room(const QString& command);

signals:
    void changed();
    void started();

private:
    LaunchFn launch_;
    std::function<void(const std::string&)> roomHandler_;
    QVariantMap room_;
    QString link_, host_, lastHost_, phase_ = QStringLiteral("idle"), message_;
    bool linkValid_ = false, streaming_ = false, access_ = false, browser_ = false;
    QString storeFile_, saved_, savedLink_;
    void saveAccess(const QString& label);
    QVariantMap stream_;
};

} // namespace veyra::ui
