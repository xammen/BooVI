#include "veyra/ui/DiscordNotifier.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <windows.h>

#include <format>

#include "veyra/Log.h"
#include "veyra/guest/GuestStreamer.h"

namespace veyra::ui {
namespace {

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
// Tests point the notices at a local fake webhook (http://127.0.0.1:<port>/…).
bool allowLocal() { return qEnvironmentVariableIntValue("VEYRA_TEST_DISCORD_LOCAL") == 1; }

void wipe(std::string& s) { if (!s.empty()) SecureZeroMemory(s.data(), s.size()); s.clear(); }

} // namespace

DiscordNotifier::DiscordNotifier(QObject* parent) : QObject(parent), net_(new QNetworkAccessManager(this)), timer_(new QTimer(this)) {
    url_ = guest::loadDiscordWebhook();
    if (!guest::webhookUrlOk(url_, allowLocal())) wipe(url_);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &DiscordNotifier::pump);
    timer_->start();
}

DiscordNotifier::~DiscordNotifier() { wipe(url_); }

bool DiscordNotifier::setWebhook(const QString& text, QString& error) {
    std::string url = text.trimmed().toStdString();
    if (!guest::webhookUrlOk(url, allowLocal())) {
        wipe(url);
        error = tr("ce n'est pas un webhook discord (https://discord.com/api/webhooks/…)");
        return false;
    }
    if (!guest::saveDiscordWebhook(url)) {
        wipe(url);
        error = tr("le webhook n'a pas pu être enregistré");
        return false;
    }
    wipe(url_);
    url_ = std::move(url);
    status_.clear();
    veyra::log::info("discord", "webhook set");
    emit changed();
    return true;
}

void DiscordNotifier::clear() {
    guest::clearDiscordWebhook();
    wipe(url_);
    status_.clear();
    veyra::log::info("discord", "webhook removed");
    emit changed();
}

void DiscordNotifier::notify(guest::Notice kind, std::string line) {
    if (url_.empty() || !(mask_ & guest::noticeBit(kind))) return;
    gate_.push(std::move(line), nowMs());
}

void DiscordNotifier::test() {
    if (url_.empty()) return;
    gate_.push("BooVI est relié à ce salon : les nouvelles du partage arriveront ici", nowMs() - guest::NotifyGate::kGatherMs);
    pump();
}

void DiscordNotifier::pump() {
    if (inFlight_ || url_.empty()) return;
    const auto message = gate_.take(nowMs());
    if (!message) return;
    QNetworkRequest request{QUrl(QString::fromStdString(url_))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(10000);
    inFlight_ = true;
    QNetworkReply* reply = net_->post(request, QByteArray::fromStdString(guest::webhookBody(*message)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, text = *message] {
        reply->deleteLater();
        inFlight_ = false;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 429) {
            const double after = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("retry_after")).toDouble(1.0);
            gate_.rateLimited(text, qint64(after * 1000), nowMs());
            veyra::log::warn("discord", std::format("429, retry in {:.1f} s", after));
            status_ = tr("discord demande d'attendre");
        } else if (code >= 200 && code < 300) {
            veyra::log::info("discord", std::format("sent ({} characters)", text.size()));
            status_ = tr("envoyé");
        } else {
            // Only the status: the URL and Discord's answer stay out of the log.
            veyra::log::warn("discord", code ? std::format("http {}", code) : std::string("no answer"));
            status_ = code ? tr("refusé (http %1)").arg(code) : tr("discord injoignable");
        }
        emit changed();
    });
}

} // namespace veyra::ui
