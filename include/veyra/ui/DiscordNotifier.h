#pragma once
// Fork, partage à plusieurs: posts the room's news to the owner's Discord channel through the
// webhook he pasted in réglages → partage (GuestNotify.h says what and how often). The URL is
// read from its DPAPI file once, kept in memory only while BooVI runs, and never logged: the log
// says "discord: sent", "discord: 429, retry in N s" or "discord: http N".
#include <QObject>
#include <QString>

#include <cstdint>
#include <string>

#include "veyra/guest/GuestNotify.h"

class QNetworkAccessManager;
class QTimer;

namespace veyra::ui {

class DiscordNotifier final : public QObject {
    Q_OBJECT
public:
    explicit DiscordNotifier(QObject* parent = nullptr);
    ~DiscordNotifier() override;

    bool configured() const { return !url_.empty(); }
    QString status() const { return status_; }   // "", "envoyé", "refusé (http 404)"…
    void setEvents(uint32_t mask) { mask_ = mask; }
    // A webhook URL typed by the owner: checked, sealed, then used. False with `error` otherwise.
    bool setWebhook(const QString& url, QString& error);
    void clear();
    void notify(guest::Notice kind, std::string line);
    void test();

signals:
    void changed();

private:
    void pump();
    QNetworkAccessManager* net_ = nullptr;
    QTimer* timer_ = nullptr;
    guest::NotifyGate gate_;
    std::string url_;
    uint32_t mask_ = guest::kDefaultNotices;
    bool inFlight_ = false;
    QString status_;
};

} // namespace veyra::ui
