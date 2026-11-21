#include "veyra/ui/GuestJoinModel.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QSaveFile>
#include <QUrl>

#include "veyra/Log.h"
#include "veyra/guest/GuestProtocol.h"
#include "veyra/ui/Dpapi.h"

namespace veyra::ui {

namespace {
const char* phaseName(source::GuestClientStats::State state) {
    switch (state) {
    case source::GuestClientStats::State::Connecting: return "connecting";
    case source::GuestClientStats::State::Streaming: return "streaming";
    case source::GuestClientStats::State::Ended: return "ended";
    case source::GuestClientStats::State::Failed: return "failed";
    default: return "idle";
    }
}
} // namespace

GuestJoinModel::GuestJoinModel(QObject* parent) : QObject(parent) {
    // Tests: VEYRA_TEST_JOIN_ROOM="ask@8,roll@14" sends those room commands that many seconds in.
    for (const QString& step : qEnvironmentVariable("VEYRA_TEST_JOIN_ROOM").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList at = step.split(QLatin1Char('@'));
        if (at.size() == 2) QTimer::singleShot(int(at[1].toDouble() * 1000), this, [this, c = at[0]] { veyra::log::info("guest-client-ui", "test: room " + c.toStdString()); room(c); });
    }
}

bool GuestJoinModel::setLink(const QString& link) {
    const auto parsed = guest::parseShareLink(link.toStdString());
    const bool valid = parsed.has_value();
    const QString host = valid ? QString::fromStdString(parsed->host) : QString();
    if (link == link_ && valid == linkValid_ && host == host_) return valid;
    link_ = link.trimmed();
    linkValid_ = valid;
    access_ = valid && parsed->access;
    host_ = host;
    emit changed();
    return valid;
}

void GuestJoinModel::setStoreFile(const QString& path) {
    storeFile_ = path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    QByteArray plain = dpapiUnseal(file.readAll());
    const auto o = QJsonDocument::fromJson(plain).object();
    plain.fill('\0');
    const QString link = o.value(QStringLiteral("link")).toString();
    const auto parsed = guest::parseShareLink(link.toStdString());
    if (!parsed || !parsed->access) return;
    savedLink_ = link;
    saved_ = o.value(QStringLiteral("label")).toString().left(40);
    if (link_.isEmpty()) setLink(link);
    veyra::log::info("guest-client-ui", "saved access loaded: " + saved_.toStdString());
    emit changed();
}

void GuestJoinModel::saveAccess(const QString& label) {
    if (storeFile_.isEmpty() || (label == saved_ && link_ == savedLink_)) return;
    const QByteArray sealed = dpapiSeal(QJsonDocument(QJsonObject{{"link", link_}, {"label", label}}).toJson(QJsonDocument::Compact));
    QSaveFile file(storeFile_);
    if (sealed.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(sealed) != sealed.size() || !file.commit()) {
        veyra::log::warn("guest-client-ui", "the access could not be saved");
        return;
    }
    saved_ = label;
    savedLink_ = link_;
    veyra::log::info("guest-client-ui", "access saved: " + label.toStdString());
    emit changed();
}

void GuestJoinModel::forgetSaved() {
    if (!storeFile_.isEmpty()) QFile::remove(storeFile_);
    if (link_ == savedLink_) { link_.clear(); linkValid_ = access_ = false; host_.clear(); }
    saved_.clear();
    savedLink_.clear();
    veyra::log::info("guest-client-ui", "saved access forgotten");
    emit changed();
}

bool GuestJoinModel::pasteLink() {
    const QString text = QGuiApplication::clipboard() ? QGuiApplication::clipboard()->text().trimmed() : QString();
    if (text.isEmpty() || !guest::parseShareLink(text.toStdString())) return false;
    return setLink(text);
}

bool GuestJoinModel::join() {
    if (!linkValid_ || !launch_) return false;
    if (!launch_(link_, host_)) return false;
    lastHost_ = host_;
    phase_ = QStringLiteral("connecting");
    message_.clear();
    veyra::log::info("guest-client-ui", "join " + host_.toStdString());
    emit changed();
    emit started();
    return true;
}

void GuestJoinModel::updateStream(bool active, const source::GuestClientStats& s) {
    const QString phase = active ? QString::fromLatin1(phaseName(s.state))
                        : (phase_ == QLatin1String("streaming") || phase_ == QLatin1String("connecting")) && s.state == source::GuestClientStats::State::Idle
                            ? QStringLiteral("idle") : QString::fromLatin1(phaseName(s.state));
    const QString message = QString::fromStdWString(s.message);
    QVariantMap stream;
    if (active) {
        stream = {{"receivedFps", s.receivedFps}, {"videoMbps", s.videoMbps}, {"dropped", qulonglong(s.dropped)},
                  {"rttMs", s.rttMs}, {"decodeMs", s.decodeMs}, {"codec", QStringLiteral("h.264")},
                  {"decoder", QString::fromStdString(s.decoder)}, {"gpu", QString::fromStdString(s.gpu)},
                  {"hostToScreenMs", s.hostToScreenMs}, {"lossPercent", s.lossPercent}, {"relayed", s.relayed},
                  {"mode", QString::fromStdString(s.controlMode)}, {"extendedInput", s.extendedInput},
                  {"width", int(s.width)}, {"height", int(s.height)},
                  {"anticipationMode", s.anticipation.mode}, {"anticipationRunning", s.anticipation.running},
                  {"anticipationLearnt", s.anticipation.learnt}, {"anticipationActive", s.anticipation.active},
                  {"anticipationConfidence", s.anticipation.confidence}, {"anticipationLagMs", s.anticipation.lagMs},
                  {"anticipationFps", s.anticipation.presentsPerSecond}};
    }
    // The host's room (RoomModel.h): who holds the controller, the queue, the dice; and this
    // client's own place in it.
    QVariantMap room;
    if (active && !s.room.empty()) {
        room = QJsonDocument::fromJson(QByteArray::fromStdString(s.room)).object().toVariantMap();
        const QString you = room.value(QStringLiteral("you")).toString();
        QString seat;
        for (const auto& m : room.value(QStringLiteral("members")).toList())
            if (m.toMap().value(QStringLiteral("id")).toString() == you) seat = m.toMap().value(QStringLiteral("seat")).toString();
        const QString role = room.value(QStringLiteral("holder")).toString() == you ? QStringLiteral("hold")
                           : room.value(QStringLiteral("queue")).toList().contains(you) ? QStringLiteral("queue")
                           : seat == QLatin1String("watcher") ? QStringLiteral("watch") : QStringLiteral("free");
        room.insert(QStringLiteral("role"), role);
        room.insert(QStringLiteral("notice"), QString::fromStdString(s.roomNotice));
    }
    if (room != room_) { room_ = room; emit changed(); }
    // No network path from here: the browser falls back to TURN over TLS on 443 on its own.
    const bool browser = phase == QLatin1String("failed") && s.iceFailed;
    const bool streaming = active && s.state == source::GuestClientStats::State::Streaming;
    if (streaming && access_) saveAccess(tr("ps5 de %1").arg(QString::fromStdString(s.hostName.empty() ? s.host : s.hostName)));
    if (phase == phase_ && message == message_ && streaming == streaming_ && stream == stream_ && browser == browser_) return;
    phase_ = phase;
    browser_ = browser;
    message_ = message;
    streaming_ = streaming;
    stream_ = stream;
    emit changed();
}

QVariantMap GuestJoinModel::state() const {
    return {{"phase", phase_}, {"streaming", streaming_}, {"linkValid", linkValid_}, {"host", host_},
            {"lastLabel", lastHost_}, {"message", message_}, {"stream", stream_}, {"access", access_}, {"saved", saved_}, {"browser", browser_}, {"room", room_}};
}

void GuestJoinModel::room(const QString& command) {
    if (roomHandler_ && streaming_) roomHandler_(command.toStdString());
}

bool GuestJoinModel::openInBrowser() {
    if (!linkValid_) return false;
    veyra::log::info("guest-client-ui", "the link opens in the browser");
    return QDesktopServices::openUrl(QUrl(link_));
}

} // namespace veyra::ui
