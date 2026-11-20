// Fork, "rejoindre un partage": the bridge side of GuestJoinModel. A share joined as a BooVI
// client opens like the PS5 stream: the engine plays it as a live source and the PS5 input
// thread (RemotePlayInput) reads this PC's pad and keyboard/mouse for it.
#include "../QmlPlayerBridge_p.h"

namespace veyra::ui {

QObject* QmlPlayerBridge::joinModel() const {
#ifdef VEYRA_ENABLE_GUEST_CLIENT
    return impl_->join.get();
#else
    return nullptr;
#endif
}

bool QmlPlayerBridge::joinLive() const {
#ifdef VEYRA_ENABLE_GUEST_CLIENT
    const auto& s = impl_->snapshot;
    return s.guestClientActive && (s.running || s.transport == engine::TransportState::Opening || s.transport == engine::TransportState::Stopping);
#else
    return false;
#endif
}

#ifdef VEYRA_ENABLE_GUEST_CLIENT
void QmlPlayerBridge::setupJoin() {
    auto& i = *impl_;
    i.join = std::make_unique<ui::GuestJoinModel>();
    i.join->setStoreFile(QString::fromStdWString((i.dataDir / L"guest-saved-access.dpapi").wstring()));
    i.join->setLaunchHandler([this](const QString& link, const QString& host) -> bool {
        auto& j = *impl_;
        if (!j.videoWindow) return false;
        rememberPosition(true);
        emit mediaOpenRequested();
        j.openingSource = true;
        j.openingSessionId = 0;
        j.sourceLabel = L"BooVI · " + host.toStdWString();
        j.resumeSession = 0;
        j.screenFillActive = false;
        auto desc = std::make_shared<source::GuestClientDesc>();
        desc->link = link.toStdString();
        openAfterCinema(QStringLiteral("BooVI · %1").arg(host), [this, desc] {
            auto& k = *impl_;
            if (k.preOpen) k.preOpen();
            k.engine.previewView({});
            k.engine.openGuestClient(k.videoWindow, std::move(*desc), k.options);
            k.openingSessionId = k.engine.snapshot().sessionId;
            refreshSubtitles({});
#ifdef VEYRA_ENABLE_REMOTEPLAY
            // The pad (SDL3: a DualSense's gyro, accel and touchpad) and keyboard/mouse, read
            // every 1 ms; tickPs5() stops the thread when the stream ends.
            // A test run with the synthetic pad (VEYRA_TEST_GUEST_CLIENT_SYNTH) leaves this PC's pads alone.
            if (!qEnvironmentVariableIsSet("VEYRA_TEST_GUEST_CLIENT_SYNTH")) {
                if (k.ps5ControlTimer) k.ps5ControlTimer->stop();
                k.releasePad(Impl::PadPs5Control);
                k.applyKbmSettings();
                k.ps5Input.setBackground(k.prefBool("ps5ControlBackground", false));
                k.ps5Input.start();
            }
#endif
        });
        rememberSource(QStringLiteral("join"), host);
        return true;
    });
}

void QmlPlayerBridge::tickJoin() {
    auto& i = *impl_;
    if (!i.join) return;
    const bool active = i.snapshot.guestClientActive;
    if (active != i.joinWasActive || (active && ++i.joinStatsTicks >= 15)) {
        i.joinWasActive = active;
        i.joinStatsTicks = 0;
        i.join->updateStream(active, i.snapshot.guestClient);
    }
}
#else
void QmlPlayerBridge::tickJoin() {}
#endif

} // namespace veyra::ui
