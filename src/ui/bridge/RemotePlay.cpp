// QML bridge: RemotePlay responsibilities.
#include "../QmlPlayerBridge_p.h"

namespace veyra::ui {
// --- PS5 Remote Play (P4-e) ---------------------------------------------------
namespace {
constexpr uint32_t kPs5Bitrates[] = {5000, 10000, 15000, 20000, 30000, 50000, 80000, 100000};
#ifdef VEYRA_ENABLE_REMOTEPLAY
std::filesystem::path ps5Settings() { return remoteplay::profileDirectory() / L"settings.ini"; }
#endif
}
QVariantMap QmlPlayerBridge::ps5() const {
    QVariantMap out = impl_->ps5Form;
    out["status"] = impl_->ps5Status;
    out["busy"] = impl_->ps5Busy;
    out["profile"] = QString::fromStdWString(impl_->ps5Profile);
#ifdef VEYRA_ENABLE_REMOTEPLAY
    out["available"] = true;
    const auto& s = impl_->snapshot;
    out["active"] = s.remotePlay && (s.running || s.transport == engine::TransportState::Opening || s.transport == engine::TransportState::Stopping);
    const auto c = impl_->ps5Pad();
    out["controller"] = c.connected;
    out["gyro"] = c.gyro && c.accel;
    out["calibrating"] = c.calibrating;
    out["pad"] = QString::fromStdString(c.name);
    out["kbm"] = impl_->kbmCapture && impl_->kbmCapture->captured();
    // The stream runs, the pad is not view-only, and nothing reaches the PS5 because no
    // Veyra window has the focus: the UI says so instead of failing silently.
    out["inputPaused"] = out["active"].toBool() && !impl_->ps5Form.value("viewOnly").toBool() && !impl_->ps5Focused;
    // The console asked for the account's login PIN: the dialog shows the PIN field only then.
    out["pinRequired"] = s.remotePlay && s.remotePlayState == int(remoteplay::SessionState::LoginPinRequired);
    // Fork, invités (GuestAccessModel.h): the picture flows, and whether the last Play woke the console.
    out["streaming"] = s.remotePlay && s.running && s.remotePlayState == int(remoteplay::SessionState::Streaming) && s.remoteDecoded > 0;
    out["woke"] = impl_->ps5LastWoke;
    // Fork: PadNotice.qml. The hint after 3 s without a pad, and a counter whose change shows the arrival toast.
    out["padHint"] = impl_->ps5PadPresence.hint();
    out["padArrivals"] = int(impl_->ps5PadPresence.arrivals());
#else
    out["available"] = false;
#endif
    return out;
}
QVariantList QmlPlayerBridge::ps5Profiles() const { return impl_->ps5ProfileList; }

// --- PC streaming (Moonlight / Sunshine) -----------------------------------------------------------
QObject* QmlPlayerBridge::moonlightModel() const {
#ifdef VEYRA_ENABLE_MOONLIGHT
    return impl_->moonlight.get();
#else
    return nullptr;
#endif
}
bool QmlPlayerBridge::moonlightCaptured() const {
#ifdef VEYRA_ENABLE_MOONLIGHT
    return impl_->moonlightCapture && impl_->moonlightCapture->captured();
#else
    return false;
#endif
}
bool QmlPlayerBridge::moonlightStatsVisible() const {
#ifdef VEYRA_ENABLE_MOONLIGHT
    return impl_->moonlightStatsVisible;
#else
    return false;
#endif
}
void QmlPlayerBridge::setMoonlightStatsVisible(bool visible) {
#ifdef VEYRA_ENABLE_MOONLIGHT
    if (impl_->moonlightStatsVisible == visible) return;
    impl_->moonlightStatsVisible = visible;
    emit moonlightUiChanged();
#else
    (void)visible;
#endif
}
void QmlPlayerBridge::openMoonlightDialog() { emit navigate(QStringLiteral("moonlight")); }

// --- Xbox home streaming (unofficial) ------------------------------------------------------------------
QObject* QmlPlayerBridge::xboxModel() const {
#ifdef VEYRA_ENABLE_XBOX
    return impl_->xbox.get();
#else
    return nullptr;
#endif
}
void QmlPlayerBridge::openXboxDialog() { emit navigate(QStringLiteral("xbox")); }
void QmlPlayerBridge::xboxDisconnect() {
#ifdef VEYRA_ENABLE_XBOX
    if (impl_->snapshot.xboxActive) stopPlayback();
#endif
}

#ifdef VEYRA_ENABLE_XBOX
void QmlPlayerBridge::setupXbox() {
    auto& i = *impl_;
    i.xbox = std::make_unique<ui::XboxModel>();
    connect(i.xbox.get(), &ui::XboxModel::notice, this, &QmlPlayerBridge::notice);
    i.xbox->setLaunchHandler([this](ui::XboxModel::Launch launch) -> bool {
        auto& j = *impl_;
        if (!j.videoWindow) return false;
        rememberPosition(true);
        emit mediaOpenRequested();
        j.openingSource = true;
        j.openingSessionId = 0;
        j.sourceLabel = L"Xbox · " + launch.label.toStdWString();
        j.resumeSession = 0;
        j.screenFillActive = false;
        const bool pad = launch.desc.gamepad;
        const QString label = launch.label;
        auto request = std::make_shared<source::XboxConnectDesc>(std::move(launch.desc));
        openAfterCinema(tr("正在连接 Xbox · %1").arg(label), [this, request, pad] {
            auto& k = *impl_;
            if (k.preOpen) k.preOpen();
            k.engine.previewView({});
            k.engine.openXbox(k.videoWindow, std::move(*request), k.options);
            k.openingSessionId = k.engine.snapshot().sessionId;
            refreshSubtitles({});
#ifdef VEYRA_ENABLE_REMOTEPLAY
            // The pad is polled on the UI thread at 250 Hz; the session sends changes and a 33 ms heartbeat.
            if (pad) {
                if (!k.acquirePad(Impl::PadXbox)) veyra::log::warn("xbox-input", "SDL gamepad initialization failed");
                if (!k.xboxTimer) {
                    k.xboxTimer = new QTimer(this);
                    k.xboxTimer->setTimerType(Qt::PreciseTimer);
                    connect(k.xboxTimer, &QTimer::timeout, this, [this] {
                        auto& m = *impl_;
                        if (!m.snapshot.xboxActive) { m.releasePad(Impl::PadXbox); m.xboxTimer->stop(); return; }
                        const bool focused = QGuiApplication::focusWindow() != nullptr;
                        m.engine.xboxController(m.controller.poll(focused));
                        m.controller.feedback(m.engine.xboxFeedback(), focused);
                    });
                }
                k.xboxTimer->start(4);
            } else {
                if (k.xboxTimer) k.xboxTimer->stop();
                k.releasePad(Impl::PadXbox);
            }
#else
            (void)pad;
#endif
        });
        rememberSource(QStringLiteral("xbox"), label);
        return true;
    });
}

void QmlPlayerBridge::tickXbox() {
    auto& i = *impl_;
    if (!i.xbox) return;
    const auto& s = i.snapshot;
    const bool active = s.xboxActive;
    if (active != i.xboxWasActive) {
        i.xboxWasActive = active;
        i.xbox->updateStream(active, s.xbox);
    } else if (active && ++i.xboxStatsTicks >= 15) {
        i.xboxStatsTicks = 0;
        i.xbox->updateStream(true, s.xbox);
    }
}
#else
void QmlPlayerBridge::tickXbox() {}
#endif

void QmlPlayerBridge::moonlightCapture(bool on) {
#ifdef VEYRA_ENABLE_MOONLIGHT
    auto& i = *impl_;
    if (!i.moonlightCapture) return;
    if (!on) {
        i.moonlightWantCapture = false;
        i.moonlightCapture->capture(false);
        return;
    }
    if (!i.snapshot.moonlightActive || !i.videoWindow) return;
    i.moonlightCapture->setWindow(GetAncestor(i.videoWindow, GA_ROOT));
    // The tick keeps trying until the window is in the foreground (right after connecting it may not be yet).
    i.moonlightWantCapture = !i.moonlightCapture->capture(true);
#else
    (void)on;
#endif
}

void QmlPlayerBridge::moonlightDisconnect() {
#ifdef VEYRA_ENABLE_MOONLIGHT
    moonlightCapture(false);
    if (impl_->snapshot.moonlightActive) stopPlayback();
#endif
}

#ifdef VEYRA_ENABLE_MOONLIGHT
void QmlPlayerBridge::setupMoonlight() {
    auto& i = *impl_;
    i.moonlight = std::make_unique<ui::MoonlightModel>();
    i.moonlightCapture = std::make_unique<ui::MoonlightInputCapture>(i.engine);
    connect(i.moonlightCapture.get(), &ui::MoonlightInputCapture::capturedChanged, this, [this](bool) { emit moonlightUiChanged(); });
    connect(i.moonlightCapture.get(), &ui::MoonlightInputCapture::releaseRequested, this, [this] {
        impl_->moonlightWantCapture = false;
        emit notice(tr("已释放键盘和鼠标（点击画面重新捕获）"), false);
    });
    connect(i.moonlightCapture.get(), &ui::MoonlightInputCapture::quitRequested, this, [this] { moonlightDisconnect(); });
    connect(i.moonlightCapture.get(), &ui::MoonlightInputCapture::statsRequested, this, [this] { setMoonlightStatsVisible(!moonlightStatsVisible()); });
    connect(i.moonlight.get(), &ui::MoonlightModel::notice, this, &QmlPlayerBridge::notice);
    i.moonlight->setLaunchHandler([this](ui::MoonlightModel::Launch launch) -> bool {
        auto& j = *impl_;
        if (!j.videoWindow) return false;
        rememberPosition(true);
        emit mediaOpenRequested();
        j.openingSource = true;
        j.openingSessionId = 0;
        j.sourceLabel = L"PC · " + launch.label.toStdWString();
        j.resumeSession = 0;
        j.screenFillActive = false;
        const bool pad = (launch.desc.options.gamepadMask & 1) != 0;
        j.moonlightWantCapture = launch.captureInput;
        const QString label = launch.label;
        auto request = std::make_shared<source::MoonlightConnectDesc>(std::move(launch.desc));
        openAfterCinema(tr("正在连接 %1").arg(label), [this, request, pad] {
            auto& k = *impl_;
            if (k.preOpen) k.preOpen();
            k.engine.previewView({});
            k.engine.openMoonlight(k.videoWindow, std::move(*request), k.options);
            k.openingSessionId = k.engine.snapshot().sessionId;
            refreshSubtitles({});
#ifdef VEYRA_ENABLE_REMOTEPLAY
            // The pad is polled on the UI thread at 250 Hz and sent only when it changes.
            if (pad) {
                if (!k.acquirePad(Impl::PadMoonlight)) veyra::log::warn("moonlight-input", "SDL gamepad initialization failed");
                if (!k.moonlightTimer) {
                    k.moonlightTimer = new QTimer(this);
                    k.moonlightTimer->setTimerType(Qt::PreciseTimer);
                    connect(k.moonlightTimer, &QTimer::timeout, this, [this] {
                        auto& m = *impl_;
                        if (!m.snapshot.moonlightActive) { m.releasePad(Impl::PadMoonlight); m.moonlightTimer->stop(); return; }
                        const bool focused = QGuiApplication::focusWindow() != nullptr;
                        m.engine.moonlightController(m.controller.poll(focused));
                        m.controller.feedback(m.engine.moonlightFeedback(), focused);
                    });
                }
                k.moonlightTimer->start(4);
            } else {
                if (k.moonlightTimer) k.moonlightTimer->stop();
                k.releasePad(Impl::PadMoonlight);
            }
#else
            (void)pad;
#endif
        });
        rememberSource(QStringLiteral("moonlight"), label);
        return true;
    });
}

void QmlPlayerBridge::tickMoonlight() {
    auto& i = *impl_;
    if (!i.moonlight) return;
    const auto& s = i.snapshot;
    const bool active = s.moonlightActive;
    if (active != i.moonlightWasActive) {
        i.moonlightWasActive = active;
        if (!active) {
            i.moonlightWantCapture = false;
            if (i.moonlightCapture) i.moonlightCapture->capture(false);
        }
        i.moonlight->updateStream(active, s.moonlight);
    } else if (active && ++i.moonlightStatsTicks >= 15) {
        i.moonlightStatsTicks = 0;
        i.moonlight->updateStream(true, s.moonlight);
    }
    // Take the keyboard and mouse as soon as the picture is up (and again when the window comes to the front).
    if (active && i.moonlightWantCapture && s.moonlight.state == source::MoonlightStats::State::Streaming) moonlightCapture(true);
}
#else
void QmlPlayerBridge::tickMoonlight() {}
#endif
void QmlPlayerBridge::ps5Load() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    try {
        const auto directory = remoteplay::profileDirectory();
        if (i.ps5Profile.empty()) {
            wchar_t last[80]{};
            GetPrivateProfileStringW(L"RemotePlay", L"LastProfile", L"", last, 80, ps5Settings().c_str());
            const std::wstring name = last;
            if (!name.empty() && name.find_first_of(L"/\\:") == std::wstring::npos) i.ps5Profile = name;
        }
        i.ps5ProfileList.clear();
        unsigned failures = 0;
        auto add = [&](const std::filesystem::path& path) {
            remoteplay::ProfileLoadError error{};
            auto saved = remoteplay::loadProfile(path, &error);
            if (saved) i.ps5ProfileList << QVariantMap{{"id", QString::fromStdWString(path.filename().wstring())},
                                                      {"label", QString::fromStdString(saved->host)}};
            else if (error != remoteplay::ProfileLoadError::Missing) ++failures;
        };
        add(directory / "remoteplay-profile.dat");
        std::error_code ec; size_t scanned = 0;
        for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end && i.ps5ProfileList.size() < 64 && scanned < 256; it.increment(ec), ++scanned) {
            const auto id = it->path().stem().wstring();
            if (it->path().extension() == ".dat" && id.size() == 32 &&
                std::all_of(id.begin(), id.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }))
                add(it->path());
        }
        // A pairing made or found in the PS5 dialog lets a waiting PS5 手柄控制 start.
        if (!i.ps5ProfileList.isEmpty()) i.ps5ControlNoPair = false;
        bool known = false;
        for (const auto& p : i.ps5ProfileList) known = known || p.toMap().value("id").toString() == QString::fromStdWString(i.ps5Profile);
        if (!known) i.ps5Profile = i.ps5ProfileList.isEmpty() ? std::wstring{} : i.ps5ProfileList.front().toMap().value("id").toString().toStdWString();
        // Form defaults, then the selected pairing's own values.
        i.ps5Form = QVariantMap{{"host", QString()}, {"account", QString()}, {"quality", 3}, {"codec", 0}, {"bitrate", 2}, {"viewOnly", false},
            {"decode", int(std::min(2u, GetPrivateProfileIntW(L"RemotePlay", L"DecodeMode", 0, ps5Settings().c_str())))},
            {"sampling", int(std::min(1u, GetPrivateProfileIntW(L"RemotePlay", L"FineSampling", 1, ps5Settings().c_str())))}};
        if (!i.ps5Profile.empty())
            if (auto saved = remoteplay::loadProfile(directory / i.ps5Profile)) {
                i.ps5Form["host"] = QString::fromStdString(saved->host);
                i.ps5Form["account"] = QString::fromStdString(remoteplay::accountIdToBase64(saved->credentials.accountId));
                i.ps5Form["quality"] = (saved->video.height == 1080 ? 2 : 0) + (saved->video.fps == 60 ? 1 : 0);
                i.ps5Form["codec"] = int(saved->video.codec);
                const auto found = std::find(std::begin(kPs5Bitrates), std::end(kPs5Bitrates), saved->video.bitrateKbps);
                i.ps5Form["bitrate"] = found == std::end(kPs5Bitrates) ? 2 : int(found - std::begin(kPs5Bitrates));
                i.ps5Form["viewOnly"] = saved->viewOnly;
            }
        if (i.ps5Status.isEmpty())
            i.ps5Status = failures ? tr("有配对存档无法读取或解密，详情见日志。请使用原 Windows 账户；不要删除原件。")
                        : !i.ps5Profile.empty() ? tr("已加载保存的主机，直接点“连接”，无需重新填写 8 位配对码。")
                        : tr("首次使用：PS5 设置 → 系统 → 远程游玩 → 关联设备，填写账号 ID 与 8 位配对码后配对。");
        veyra::log::info("remoteplay-ui", std::format("profiles={} readFailures={}", i.ps5ProfileList.size(), failures));
    } catch (...) {
        impl_->ps5Status = tr("无法读取远程游玩目录；原有配对文件不会被改动。");
    }
#else
    impl_->ps5Status = tr("此版本未包含 PS5 远程游玩组件。");
#endif
    emit ps5Changed();
}
bool QmlPlayerBridge::ps5Set(const QString& key, const QVariant& value) {
    static const QHash<QString, std::pair<int, int>> ranges{{"quality", {0, 3}}, {"codec", {0, 2}}, {"bitrate", {0, 7}}, {"decode", {0, 2}}, {"sampling", {0, 1}}};
    if (ranges.contains(key)) {
        bool ok = false; const int n = value.toInt(&ok); const auto [low, high] = ranges.value(key);
        if (!ok || n < low || n > high) return false;
        impl_->ps5Form[key] = n;
    } else if (key == QLatin1String("viewOnly")) impl_->ps5Form[key] = value.toBool();
    else if (key == QLatin1String("host") || key == QLatin1String("account")) impl_->ps5Form[key] = value.toString().trimmed();
    else return false;
    emit ps5Changed();
    return true;
}
void QmlPlayerBridge::ps5SelectProfile(const QString& id) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    if (impl_->ps5Busy) return;
    impl_->ps5Profile = id.toStdWString();
    WritePrivateProfileStringW(L"RemotePlay", L"LastProfile", impl_->ps5Profile.c_str(), ps5Settings().c_str());
    impl_->ps5Status.clear();
    ps5Load();
    if (impl_->engine.remoteControlRequested()) startPs5Control(true);
#else
    Q_UNUSED(id);
#endif
}
#ifdef VEYRA_ENABLE_REMOTEPLAY
namespace {
remoteplay::VideoProfile ps5Video(const QVariantMap& f) {
    remoteplay::VideoProfile p; const int q = f.value("quality").toInt();
    p.width = q < 2 ? 1280 : 1920; p.height = q < 2 ? 720 : 1080; p.fps = (q == 0 || q == 2) ? 30 : 60;
    p.codec = static_cast<remoteplay::Codec>(std::clamp(f.value("codec").toInt(), 0, 2));
    p.bitrateKbps = kPs5Bitrates[std::clamp(f.value("bitrate").toInt(), 0, 7)];
    return p;
}
std::string asciiOnly(const QString& s) {
    std::string out;
    for (const QChar c : s) { if (c.unicode() > 127) return {}; out.push_back(char(c.unicode())); }
    return out;
}
}
#endif
// One worker at a time, as the 1.4.4 panel; results are picked up in tickPs5().
#define VEYRA_PS5_LAUNCH(body) \
    do { if (impl_->ps5Busy) return; if (impl_->ps5Worker.joinable()) impl_->ps5Worker.join(); \
         impl_->ps5Busy = true; impl_->ps5Done = false; impl_->ps5Status = tr("正在处理…可随时取消"); emit ps5Changed(); \
         impl_->ps5Worker = std::jthread([this, fn = std::move(body)](std::stop_token stop) mutable { \
             Impl::Ps5Outcome result; \
             try { result = fn(stop); } catch (...) { result.message = L"操作失败；请检查网络、输入和可用磁盘空间。"; } \
             { std::lock_guard lock(impl_->ps5Mutex); impl_->ps5Outcome = std::move(result); } impl_->ps5Done = true; }); } while (0)
void QmlPlayerBridge::ps5Scan() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto work = [](std::stop_token stop) {
        Impl::Ps5Outcome out; auto report = remoteplay::discoverLocalPs5(stop);
        for (const auto& line : report.diagnostics) veyra::log::info("remoteplay-discovery", line);
        for (auto& h : report.hosts) {
            std::string id = h.consoleId;
            std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            out.hosts.push_back({h.host, id});
        }
        out.message = stop.stop_requested() ? L"已取消查找" : out.hosts.empty()
            ? (report.error ? std::format(L"主机搜索发生错误（{}），详情见日志；可手填 IP。", report.error) : L"搜索完成，未发现 PS5；确认同一局域网，或手动填写 IP。")
            : std::format(L"发现 {} 台 PS5。", out.hosts.size());
        return out;
    };
    VEYRA_PS5_LAUNCH(work);
#endif
}
void QmlPlayerBridge::ps5Play() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (i.ps5Busy) return;
    auto saved = i.ps5Profile.empty() ? std::nullopt : remoteplay::loadProfile(remoteplay::profileDirectory() / i.ps5Profile);
    const auto host = asciiOnly(i.ps5Form.value("host").toString());
    // Not paired or no address: ps5Connect says what is missing.
    if (!saved || !remoteplay::validHost(host)) { if (!ps5Connect()) emit notice(i.ps5Status, true); return; }
    saved->host = host;
    auto work = [request = std::move(*saved), host](std::stop_token stop) {
        Impl::Ps5Outcome out;
        out.connectAfter = true;
        // 0 no answer, 1 rest mode, 2 ready. A unicast probe of the saved address answers in
        // about 0.1 s; the broadcast sweep took about 6 s per check (QuickProbe.h).
        auto state = [&] {
            switch (remoteplay::probePs5(host)) {
            case remoteplay::ProbeState::Ready: return 2;
            case remoteplay::ProbeState::RestMode: return 1;
            default: return 0;
            }
        };
        if (state() != 1) return out;   // ready, or not answering: connect and let the session report
        const auto wake = remoteplay::wakeLocalPs5(request);
        if (!wake.ok) {
            out.connectAfter = false;
            out.message = std::format(L"唤醒失败（{}）；确认 PS5 处于待机而不是关机。", wake.code);
            return out;
        }
        veyra::log::info("remoteplay-wake", "wake sent; waiting for the console to report ready");
        for (int check = 1; check <= 120 && !stop.stop_requested(); ++check) {
            if (state() == 2) {
                veyra::log::info("remoteplay-wake", std::format("console ready after {} checks", check));
                out.woke = true;
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        out.connectAfter = false;
        out.message = stop.stop_requested() ? L"已取消唤醒" : L"PS5 没有在 60 秒内醒来，请检查主机后重试。";
        return out;
    };
    VEYRA_PS5_LAUNCH(work);
    i.ps5Status = tr("正在检查 PS5，必要时唤醒…");
    emit ps5Changed();
#else
    if (!ps5Connect()) emit notice(impl_->ps5Status, true);
#endif
}
void QmlPlayerBridge::ps5Pair(const QString& pinText) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto host = asciiOnly(impl_->ps5Form.value("host").toString()), id = asciiOnly(impl_->ps5Form.value("account").toString()), pin = asciiOnly(pinText);
    auto account = remoteplay::accountIdFromBase64(id); if (!account) account = remoteplay::accountIdFromDecimal(id);
    if (!remoteplay::validHost(host) || !account || !remoteplay::parsePairingPin(pin)) {
        impl_->ps5Status = tr("请填写有效主机地址、Account ID 和 8 位配对码。"); emit ps5Changed(); return;
    }
    // Same host + account reuses its file; otherwise a fresh random name.
    std::filesystem::path target;
    for (const auto& p : impl_->ps5ProfileList) {
        const auto path = remoteplay::profileDirectory() / p.toMap().value("id").toString().toStdWString();
        if (auto saved = remoteplay::loadProfile(path); saved && saved->host == host && saved->credentials.accountId == *account) target = path;
    }
    if (target.empty()) {
        GUID guid{}; if (FAILED(CoCreateGuid(&guid))) { impl_->ps5Status = tr("无法分配配对存档，请重试。"); emit ps5Changed(); return; }
        std::string name; const auto* bytes = reinterpret_cast<const unsigned char*>(&guid);
        for (size_t k = 0; k < sizeof(guid); ++k) name += std::format("{:02x}", bytes[k]);
        target = remoteplay::profileDirectory() / (name + ".dat");
    }
    const auto format = ps5Video(impl_->ps5Form);
    auto work = [host = std::move(host), account = *account, pin = std::move(pin), format, target](std::stop_token stop) mutable {
        auto result = remoteplay::pairLocalPs5(host, account, pin, stop); SecureZeroMemory(pin.data(), pin.size());
        Impl::Ps5Outcome out;
        if (result.result.ok && !stop.stop_requested()) {
            remoteplay::NativeConnectRequest request; request.host = host; request.video = format; request.credentials = std::move(result.credentials);
            for (auto byte : result.mac) request.consoleId += std::format("{:02x}", byte);
            if (request.consoleId == "000000000000") request.consoleId.clear();
            if (remoteplay::saveProfile(target, request)) { out.savedPath = target.filename().wstring(); out.message = L"配对成功并已加密保存；现在可以连接。"; }
            else out.message = L"配对成功，但保存失败；请检查用户目录权限。";
        } else out.message = result.canceled || stop.stop_requested() ? L"已取消配对" : std::format(L"配对失败（{}），检查 PS5 配对码、Account ID 与网络。", result.result.code);
        return out;
    };
    VEYRA_PS5_LAUNCH(work);
#else
    Q_UNUSED(pinText);
#endif
}
bool QmlPlayerBridge::ps5Connect() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (i.ps5Busy) return false;
    const bool tapAfterWake = std::exchange(i.ps5TapNextConnect, false);
    const auto path = i.ps5Profile.empty() ? std::filesystem::path{} : remoteplay::profileDirectory() / i.ps5Profile;
    auto saved = path.empty() ? std::nullopt : remoteplay::loadProfile(path);
    const auto host = asciiOnly(i.ps5Form.value("host").toString());
    if (!saved || !remoteplay::validHost(host)) { i.ps5Status = tr("请先完成配对，并填写有效主机地址。"); emit ps5Changed(); return false; }
    saved->host = host; saved->video = ps5Video(i.ps5Form); saved->viewOnly = i.ps5Form.value("viewOnly").toBool();
    if (!remoteplay::saveProfile(path, *saved)) { i.ps5Status = tr("保存连接设置失败，请检查目录权限。"); emit ps5Changed(); return false; }
    source::RemotePlayConnectDesc desc; desc.request = std::move(*saved);
    const int decode = std::clamp(i.ps5Form.value("decode").toInt(), 0, 2);
    desc.decodeMode = static_cast<source::RemotePlayConnectDesc::DecodeMode>(decode);
    desc.highQualitySampling = i.ps5Form.value("sampling").toInt() == 1;
    WritePrivateProfileStringW(L"RemotePlay", L"DecodeMode", std::to_wstring(decode).c_str(), ps5Settings().c_str());
    WritePrivateProfileStringW(L"RemotePlay", L"FineSampling", desc.highQualitySampling ? L"1" : L"0", ps5Settings().c_str());
    const bool viewOnly = desc.request.viewOnly;
    // The console has one Remote Play slot; the engine ends PS5 手柄控制 first.
    if (i.engine.remoteControlRequested()) emit notice(tr("已结束 PS5 手柄控制：PS5 同一时间只允许一个远程游玩连接"), false);
    rememberPosition(true);
    // The session id is cleared with the flag: the previous session's id left over here
    // ended "opening" on the next snapshot while the old source still ran.
    emit mediaOpenRequested();
    i.openingSource = true; i.openingSessionId = 0; i.sourceLabel = L"PS5 · " + std::wstring(host.begin(), host.end()); i.resumeSession = 0; i.screenFillActive = false;
    auto request = std::make_shared<source::RemotePlayConnectDesc>(std::move(desc));
    openAfterCinema(tr("正在连接 PS5 · %1").arg(QString::fromStdString(host)), [this, request, viewOnly, tapAfterWake] {
        auto& i = *impl_;
        if (i.preOpen) i.preOpen();
        i.engine.previewView({});
        i.engine.openRemotePlay(i.videoWindow, std::move(*request), i.options);
        i.openingSessionId = i.engine.snapshot().sessionId;
        i.ps5TapSession = tapAfterWake && !viewOnly ? i.openingSessionId : 0;
        i.ps5TapAt = 0;
        refreshSubtitles({});
        // View-only leaves the pad to the PS5. Otherwise the input thread reads and sends
        // the pad and keyboard/mouse; tickPs5() stops it when the stream ends.
        if (!viewOnly) {
            // PS5 手柄控制 ends with this stream; its UI-thread pad reader stops now, not on its next tick.
            if (i.ps5ControlTimer) i.ps5ControlTimer->stop();
            i.releasePad(Impl::PadPs5Control);
            i.applyKbmSettings();
            i.ps5Input.setBackground(i.prefBool("ps5ControlBackground", false));
            i.ps5Input.start();
        } else {
            i.ps5Input.stop();
        }
    });
    i.ps5Watching = true;
    rememberSource(QStringLiteral("ps5"), QString::fromStdString(host));
    i.ps5Status = tr("连接已开始。需要登录 PIN 时在下方提交。关闭面板不停止串流。");
    veyra::log::info("remoteplay-ui", std::format("connect quality={} codec={} bitrate={} decode={} viewOnly={}",
        i.ps5Form.value("quality").toInt(), i.ps5Form.value("codec").toInt(), i.ps5Form.value("bitrate").toInt(), decode, viewOnly));
    emit ps5Changed();
    return true;
#else
    return false;
#endif
}
void QmlPlayerBridge::ps5Wake() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto saved = impl_->ps5Profile.empty() ? std::nullopt : remoteplay::loadProfile(remoteplay::profileDirectory() / impl_->ps5Profile);
    if (!saved) { impl_->ps5Status = tr("需要先配对才能唤醒。"); emit ps5Changed(); return; }
    saved->host = asciiOnly(impl_->ps5Form.value("host").toString());
    auto work = [request = std::move(*saved)](std::stop_token stop) {
        Impl::Ps5Outcome out; if (stop.stop_requested()) { out.message = L"已取消唤醒"; return out; }
        auto r = remoteplay::wakeLocalPs5(request);
        out.message = r.ok ? L"已发送唤醒请求，等待 PS5 开机后再连接。" : std::format(L"唤醒失败（{}）；确认 PS5 处于待机而不是关机。", r.code);
        return out;
    };
    VEYRA_PS5_LAUNCH(work);
#endif
}
// Fork, invités: the live PS5 session asks the console to enter rest mode (chiaki goto_bed).
bool QmlPlayerBridge::ps5GotoBed() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    const bool sent = impl_->engine.remotePlayGotoBed();
    veyra::log::info("remoteplay-ui", std::format("rest mode {}", sent ? "requested" : "not requested (no live session)"));
    return sent;
#else
    return false;
#endif
}
void QmlPlayerBridge::ps5Forget() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    if (impl_->ps5Busy || impl_->ps5Profile.empty()) return;
    std::error_code ec;
    const bool removed = std::filesystem::remove(remoteplay::profileDirectory() / impl_->ps5Profile, ec);
    impl_->ps5Status = removed ? tr("配对已删除。现有串流会保留；下次连接需选择其他主机或重新配对。") : tr("删除失败，请检查目录权限。");
    if (removed) impl_->ps5Profile.clear();
    ps5Load();
#endif
}
void QmlPlayerBridge::ps5OpenAccountChecker() {
    // Fixed public URL; no account IDs, pairing credentials or login tokens leave Veyra.
    const bool opened=QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.psntools.com/psn/checker")));
    impl_->ps5Status=opened
        ? tr("在网页获取 PSN Account ID 后，复制到配对栏，再填写主机的 8 位配对码。")
        : tr("无法打开浏览器，请检查默认浏览器设置。");
    emit ps5Changed();
}
void QmlPlayerBridge::ps5SendLoginPin(const QString& pin) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    const auto text = pin.trimmed();
    if (text.isEmpty() || !std::all_of(text.begin(), text.end(), [](QChar c) { return c.isDigit(); })) {
        impl_->ps5Status = tr("登录 PIN 必须为数字。"); emit ps5Changed(); return;
    }
    impl_->engine.remotePlayLoginPin(text.toStdString());
    // Fork: remembered (DPAPI, this Windows user) only when the user turned it on.
    if (impl_->prefBool("ps5RememberPin", false) && !impl_->ps5Profile.empty()) {
        if (!remoteplay::saveLoginPin(remoteplay::profileDirectory() / impl_->ps5Profile, text.toStdString()))
            veyra::log::warn("remoteplay-ui", "login PIN not remembered: encryption or write failed");
    }
#else
    (void)pin; // RemotePlay disabled: keep the QML-callable symbol but no backend.
#endif
}
void QmlPlayerBridge::ps5Cancel() {
    if (impl_->ps5Busy && impl_->ps5Worker.joinable()) { impl_->ps5Worker.request_stop(); return; }
    if (impl_->snapshot.remotePlay) stopPlayback();
}
void QmlPlayerBridge::ps5Calibrate() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    bool started = false;
    if (!i.ps5Input.running()) started = i.controller.calibrate();
    else if (const auto pad = i.ps5Input.capabilities(); pad.connected && pad.gyro && pad.accel) { i.ps5Input.calibrate(); started = true; }
    i.ps5Status = started ? tr("将手柄平放并保持静止：采集 120 个稳定样本完成校准；10 秒内不稳定则保留原校准。")
                          : tr("请先连接串流及带陀螺仪的电脑手柄。仅观看模式不使用电脑手柄。");
    emit ps5Changed();
#endif
}
void QmlPlayerBridge::ps5PadHintClose() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    impl_->ps5PadPresence.close();
    emit ps5Changed();
#endif
}
void QmlPlayerBridge::ps5Kbm(bool on) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (!i.kbmCapture) {
        i.kbmCapture = std::make_unique<ui::MoonlightInputCapture>();
        connect(i.kbmCapture.get(), &ui::MoonlightInputCapture::capturedChanged, this, [this](bool captured) {
            auto& j = *impl_;
            j.ps5Input.setKbmWindow(captured && j.videoWindow ? GetAncestor(j.videoWindow, GA_ROOT) : nullptr);
            emit ps5Changed();
        });
        connect(i.kbmCapture.get(), &ui::MoonlightInputCapture::releaseRequested, this, [this] {
            emit notice(tr("已释放键盘和鼠标（点击画面重新捕获）"), false);
        });
        connect(i.kbmCapture.get(), &ui::MoonlightInputCapture::quitRequested, this, [this] { ps5Cancel(); });
        connect(i.kbmCapture.get(), &ui::MoonlightInputCapture::keysRequested, this, &QmlPlayerBridge::ps5KeysRequested);
    }
    if (!on) { i.kbmCapture->capture(false); return; }
    i.applyKbmSettings();
    // A joined share takes the keyboard and mouse like the PS5 stream (its host decides who plays).
    const bool joined = joinLive();
    if ((!i.snapshot.remotePlay && !joined) || !i.videoWindow || (!joined && i.ps5Form.value("viewOnly").toBool())) return;
    i.kbmCapture->setWindow(GetAncestor(i.videoWindow, GA_ROOT));
    if (!i.kbmCapture->capture(true)) veyra::log::warn("remoteplay-input", "keyboard/mouse capture refused: Veyra is not the foreground window");
    else veyra::log::info("remoteplay-input", "keyboard/mouse captured for the PS5");
#else
    (void)on;
#endif
}
QVariantList QmlPlayerBridge::ps5KbmBindings() const {
    QVariantList out;
#ifdef VEYRA_ENABLE_REMOTEPLAY
    using B = remoteplay::ControllerState;
    using T = remoteplay::KbmTarget;
    struct Control { uint32_t bit; const char* group; const char* id; };
    static constexpr Control buttons[] = {
        {B::Cross, "actions", "cross"}, {B::Circle, "actions", "circle"}, {B::Square, "actions", "square"},
        {B::Triangle, "actions", "triangle"}, {B::L3, "actions", "l3"}, {B::R3, "actions", "r3"},
        {B::Up, "dpad", "up"}, {B::Down, "dpad", "down"}, {B::Left, "dpad", "left"}, {B::Right, "dpad", "right"},
        {B::L1, "triggers", "l1"}, {B::R1, "triggers", "r1"},
        {B::Options, "system", "options"}, {B::Share, "system", "create"}, {B::Touchpad, "system", "touchpad"}, {B::PS, "system", "ps"}};
    const auto control = [](const remoteplay::KbmBinding& b) -> std::pair<const char*, const char*> {
        switch (b.target) {
        case T::LeftUp: return {"move", "ls-up"};
        case T::LeftDown: return {"move", "ls-down"};
        case T::LeftLeft: return {"move", "ls-left"};
        case T::LeftRight: return {"move", "ls-right"};
        case T::L2: return {"triggers", "l2"};
        case T::R2: return {"triggers", "r2"};
        case T::Button: for (const auto& c : buttons) if (b.button == c.bit) return {c.group, c.id}; break;
        case T::None: break;
        }
        return {nullptr, nullptr};
    };
    const bool gyro = impl_->prefs.value("kbmMouse").toString() == QLatin1String("gyro");
    out.push_back(QVariantMap{{"group", "camera"}, {"control", gyro ? "gyro" : "rs"}, {"mouse", "move"}});
    for (const auto& bound : remoteplay::KbmMapper().bindings()) {
        const auto [group, id] = control(bound.binding);
        if (!group) continue;
        QVariantMap row{{"group", group}, {"control", id}};
        if (bound.mouse != remoteplay::KbmMouse{}) {
            static constexpr const char* mouse[] = {"", "left", "middle", "right", "x1", "x2", "wheel-up", "wheel-down"};
            row["mouse"] = mouse[size_t(bound.mouse)];
        } else {
            // The key's name on this keyboard's layout (Z on AZERTY where QWERTY has W).
            wchar_t name[64]{};
            const LONG param = LONG(bound.scan << 16) | (bound.extended ? 1L << 24 : 0L);
            row["key"] = GetKeyNameTextW(param, name, 64) > 0 ? QString::fromWCharArray(name) : QStringLiteral("0x%1").arg(bound.scan, 2, 16, QLatin1Char('0'));
        }
        out.push_back(row);
    }
#endif
    return out;
}
void QmlPlayerBridge::tickPs5() {
    auto& i = *impl_;
    bool changed = false;
    if (i.ps5Busy && i.ps5Done.load()) {
        if (i.ps5Worker.joinable()) i.ps5Worker.join();
        i.ps5Busy = false;
        Impl::Ps5Outcome r; { std::lock_guard lock(i.ps5Mutex); r = std::move(i.ps5Outcome); }
        i.ps5Status = veyra::ui::i18n::text(r.message);
        if (r.connectAfter) i.ps5LastWoke = r.woke;
        if (r.connectAfter) QTimer::singleShot(0, this, [this, woke = r.woke] {
            impl_->ps5TapNextConnect = woke;
            if (!ps5Connect()) emit notice(tr("没能继续上次的 PS5 串流：%1").arg(impl_->ps5Status), true);
        });
#ifdef VEYRA_ENABLE_REMOTEPLAY
        if (!r.savedPath.empty()) {
            i.ps5Profile = r.savedPath;
            WritePrivateProfileStringW(L"RemotePlay", L"LastProfile", i.ps5Profile.c_str(), ps5Settings().c_str());
            const auto status = i.ps5Status; ps5Load(); i.ps5Status = status;
        }
        if (!r.hosts.empty()) {
            // A found console updates the selected pairing's address only when its
            // id matches (or the legacy profile had no id and the same host).
            const auto path = i.ps5Profile.empty() ? std::filesystem::path{} : remoteplay::profileDirectory() / i.ps5Profile;
            auto selected = path.empty() ? std::nullopt : remoteplay::loadProfile(path);
            bool matched = false;
            if (selected) for (const auto& [host, id] : r.hosts) {
                if ((!selected->consoleId.empty() && selected->consoleId == id) || (selected->consoleId.empty() && selected->host == host)) {
                    selected->host = host; if (id.size() == 12) selected->consoleId = id;
                    if (remoteplay::saveProfile(path, *selected)) { i.ps5Form["host"] = QString::fromStdString(host); matched = true; }
                    break;
                }
            }
            if (!selected && r.hosts.size() == 1) i.ps5Form["host"] = QString::fromStdString(r.hosts.front().first);
            else if (selected && !matched) i.ps5Status = tr("发现主机，但无法确认是已配对的这一台。保留原地址，可核对后手动修改。");
            else if (!selected && r.hosts.size() > 1) i.ps5Status = tr("发现多台 PS5，请填写要配对的主机 IP，避免选错主机。");
        }
#endif
        changed = true;
    }
#ifdef VEYRA_ENABLE_REMOTEPLAY
    // Fork: answer the console's login PIN request with the remembered PIN, once per session.
    if (i.snapshot.remotePlay && i.snapshot.remotePlayState == int(remoteplay::SessionState::LoginPinRequired)
        && i.ps5PinSession != i.snapshot.sessionId && !i.ps5Profile.empty() && i.prefBool("ps5RememberPin", false)) {
        i.ps5PinSession = i.snapshot.sessionId;
        if (auto pin = remoteplay::loadLoginPin(remoteplay::profileDirectory() / i.ps5Profile)) {
            veyra::log::info("remoteplay-ui", "remembered login PIN sent");
            i.engine.remotePlayLoginPin(*pin);
            SecureZeroMemory(pin->data(), pin->size());
        }
    }
    if (i.ps5Watching && !i.ps5Busy) {
        const auto& s = i.snapshot;
        const bool active = s.remotePlay && (s.running || s.transport == engine::TransportState::Opening || s.transport == engine::TransportState::Stopping);
        QString message = veyra::ui::i18n::text(s.failed ? s.status : s.remoteRecovering ? s.remoteRecoveryMessage
                          : s.remotePlay && s.running ? std::wstring(L"PS5 画面已进入播放；关闭此面板不停止串流。") : active ? s.status : std::wstring(L"PS5 串流已停止，可以重新连接。"));
        if (s.remotePlay && s.running && !s.failed && !s.remoteRecovering) {
            const auto& r = s.remoteStream; const auto& p = r.requestedProfile;
            message = veyra::ui::i18n::text(std::format(L"生效：{}p / {} fps / {} / 请求 {} Mbps · 实收视频 {:.1f} Mbps", p.height, p.fps,
                p.codec == remoteplay::Codec::H264 ? L"H.264" : p.codec == remoteplay::Codec::H265Hdr ? L"H.265 HDR" : L"H.265", p.bitrateKbps / 1000, r.videoMbps));
            const auto c = i.ps5Pad();
            if (i.ps5Form.value("viewOnly").toBool()) message += tr("\n仅观看：电脑输入已关闭，手柄由 PS5 处理。");
            else if (!c.connected) message += tr("\n未检测到电脑手柄。");
            else message += veyra::ui::i18n::text(std::format(L"\n陀螺仪 {} · 触摸板 {} · 扳机 {} · 触觉 {}{}", c.gyro && c.accel ? L"已启用" : L"不可用",
                c.touch ? L"可用" : L"不可用", c.triggers ? L"已接入" : L"不可用", c.haptics ? L"端点已打开" : L"未打开", c.calibrating ? L" · 校准中" : L""));
        }
        if (message != i.ps5Status) { i.ps5Status = message; changed = true; }
        if (!active && !s.remotePlay) i.ps5Watching = false;
    }
    if (i.ps5TapSession && i.snapshot.sessionId == i.ps5TapSession && i.ps5Input.running()
        && i.snapshot.remotePlayState == int(remoteplay::SessionState::Streaming) && i.snapshot.remoteDecoded > 0) {
        const uint64_t now = GetTickCount64();
        if (!i.ps5TapAt) i.ps5TapAt = now + 1500;
        else if (now >= i.ps5TapAt) {
            i.ps5TapSession = 0;
            i.ps5Input.tapPs();
            veyra::log::info("remoteplay-wake", "PS button tapped after wake");
        }
    }
    {
        // A pad plugged in or lost mid-stream refreshes every pad chip at once.
        const auto pad = i.ps5Pad();
        if (pad != i.ps5PadSeen) { i.ps5PadSeen = pad; changed = true; }
        const auto& s = i.snapshot;
        const bool live = s.remotePlay && s.running && s.remotePlayState == int(remoteplay::SessionState::Streaming) && s.remoteDecoded > 0
                       && i.ps5Input.running() && !i.ps5Form.value("viewOnly").toBool();
        const bool kbm = i.kbmCapture && i.kbmCapture->captured();
        if (i.ps5PadPresence.update(live, kbm, pad.connected, pad.name, GetTickCount64())) changed = true;
    }
    if (i.ps5Input.running()) {
        const auto& s = i.snapshot;
        if ((s.remotePlay && (s.running || s.transport == engine::TransportState::Opening)) || joinLive()) {
            i.ps5Input.setBackground(i.prefBool("ps5ControlBackground", false));
            if (const bool focused = i.ps5Input.focused(); focused != i.ps5Focused) { i.ps5Focused = focused; changed = true; }
        } else {
            if (i.kbmCapture) i.kbmCapture->capture(false);
            i.ps5Input.stop();
            i.ps5Focused = true;
            changed = true;
        }
    }
#endif
    if (changed) emit ps5Changed();
    tickPs5Control();
    testStallUi();
}
// Tests only: VEYRA_TEST_UI_STALL="<ms>,<period ms>" blocks the GUI thread for <ms> every
// <period ms> while a PS5 stream runs, to measure how input copes with a stalled UI.
void QmlPlayerBridge::testStallUi() {
    static const auto spec = [] {
        const auto parts = qEnvironmentVariable("VEYRA_TEST_UI_STALL").split(',');
        const auto value = parts.size() == 2 ? std::pair{std::clamp(parts[0].toInt(), 0, 2000), std::clamp(parts[1].toInt(), 100, 60000)} : std::pair{0, 0};
        if (value.first) veyra::log::warn("remoteplay-input", std::format("test: GUI stalls of {} ms every {} ms during a PS5 stream", value.first, value.second));
        return value;
    }();
    static int64_t next = 0;
    if (!spec.first || !impl_->snapshot.remotePlay) return;
    const int64_t now = int64_t(GetTickCount64());
    if (!next) next = now + spec.second;
    if (now < next) return;
    next = now + spec.second;
    veyra::log::warn("remoteplay-input", std::format("test: GUI thread stalled for {} ms", spec.first));
    Sleep(DWORD(spec.first));
}

// --- PS5 手柄控制 (capture card dialog) ----------------------------------------
QVariantMap QmlPlayerBridge::ps5Control() const {
    QVariantMap out = impl_->ps5ControlState;
    if (!out.contains(QStringLiteral("state"))) out[QStringLiteral("state")] = QStringLiteral("off");
#ifdef VEYRA_ENABLE_REMOTEPLAY
    out[QStringLiteral("available")] = true;
#else
    out[QStringLiteral("available")] = false;
#endif
    out[QStringLiteral("enabled")] = impl_->prefBool("ps5CaptureControl", false);
    out[QStringLiteral("background")] = impl_->prefBool("ps5ControlBackground", false);
    return out;
}
// liveKind is "ps5", "screen", "moonlight" or "xbox" for those sources; a file
// leaves it alone but does not set snapshot.capture.
bool QmlPlayerBridge::captureCardLive() const {
    const auto& s = impl_->snapshot;
    // Not while Stopping: after 断开 the run keeps `running` until it has torn
    // down, and the tick would start PS5 手柄控制 again in that window.
    const bool playing = s.running && !s.image && !s.remotePlay &&
                         (s.transport == engine::TransportState::Playing || s.transport == engine::TransportState::Paused);
    // Tests only: any playing source stands in for the capture card (no card needed).
    if (playing && qEnvironmentVariableIsSet("VEYRA_TEST_PS5_CONTROL_ANY_SOURCE")) return true;
    return playing && s.capture && impl_->liveKind == QLatin1String("capture");
}
void QmlPlayerBridge::ps5ControlSet(const QString& key, const QVariant& value) {
    static const QHash<QString, QString> keys{{QStringLiteral("enabled"), QStringLiteral("ps5CaptureControl")},
                                              {QStringLiteral("background"), QStringLiteral("ps5ControlBackground")}};
    if (!keys.contains(key)) return;
    const bool on = value.toBool();
    const auto previous = impl_->prefs;
    impl_->prefs[keys.value(key)] = on;
    if (!impl_->savePrefs()) { impl_->prefs = previous; emit notice(tr("PS5 手柄控制设置保存失败"), true); emit ps5ControlChanged(); return; }
    veyra::log::info("remoteplay-control", std::format("ui {}={}", key.toStdString(), on));
    if (key == QLatin1String("enabled")) {
        impl_->ps5ControlNoPair = false;
        if (!on) stopPs5Control();
        else if (captureCardLive()) startPs5Control(false);
        tickPs5Control(true);
    }
    emit ps5ControlChanged();
}
void QmlPlayerBridge::ps5ControlRestart() {
    if (impl_->prefBool("ps5CaptureControl", false) && captureCardLive()) startPs5Control(true);
}
void QmlPlayerBridge::startPs5Control(bool restart) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    source::RemotePlayConnectDesc desc;
    std::wstring profile;
    // Tests only: a dummy pairing to this host (an unroutable test address); the
    // saved pairings are not read, so no real console is ever contacted.
    const QString testHost = qEnvironmentVariable("VEYRA_TEST_PS5_CONTROL_HOST");
    if (!testHost.isEmpty()) {
        desc.request.host = testHost.toStdString();
        profile = L"test:" + testHost.toStdWString();
    } else {
        if (i.ps5Profile.empty()) ps5Load();
        auto saved = i.ps5Profile.empty() ? std::nullopt : remoteplay::loadProfile(remoteplay::profileDirectory() / i.ps5Profile);
        if (!saved) {
            i.engine.stopRemoteControl();
            if (!i.ps5ControlNoPair) emit notice(tr("PS5 手柄控制：还没有配对的 PS5，先在「PS5 串流」里配对一次"), true);
            i.ps5ControlNoPair = true;
            tickPs5Control(true);
            return;
        }
        desc.request = std::move(*saved);
        profile = i.ps5Profile;
    }
    const auto running = i.engine.remoteControlStatus();
    if (!restart && running.requested && (running.alive || running.starting) && profile == i.ps5ControlProfile) {
        if (!(i.padUsers & Impl::PadPs5Control)) i.acquirePad(Impl::PadPs5Control);
        if (i.ps5ControlTimer && !i.ps5ControlTimer->isActive()) i.ps5ControlTimer->start(8);
        return;
    }
    // Keepalive profile, the lightest the console accepts. Never written back to the pairing.
    remoteplay::VideoProfile keepalive;
    keepalive.width = 1280; keepalive.height = 720; keepalive.fps = 30; keepalive.bitrateKbps = 5000; keepalive.codec = remoteplay::Codec::H264;
    desc.request.video = keepalive;
    veyra::log::info("remoteplay-control", std::format("start host={} restart={} background={}", desc.request.host, restart, i.prefBool("ps5ControlBackground", false)));
    i.ps5ControlProfile = profile;
    i.ps5ControlNoticed.clear();
    i.ps5ControlNoPair = false;
    i.engine.startRemoteControl(std::move(desc));
    // The pad is read on the UI thread every 8 ms, as for the PS5 stream (the
    // session sends one state per 4 ms and queues the rest).
    if (!i.acquirePad(Impl::PadPs5Control)) veyra::log::warn("remoteplay-input", "SDL gamepad initialization failed");
    if (!i.ps5ControlTimer) {
        i.ps5ControlTimer = new QTimer(this);
        connect(i.ps5ControlTimer, &QTimer::timeout, this, [this] {
            auto& j = *impl_;
            // A PS5 stream reads the pad on its own input thread and takes the slot.
            if (j.snapshot.remotePlay || !j.engine.remoteControlRequested()) {
                j.ps5ControlTimer->stop(); j.releasePad(Impl::PadPs5Control); return;
            }
            const bool focused = QGuiApplication::focusWindow() != nullptr || j.prefBool("ps5ControlBackground", false);
            j.engine.remotePlayController(j.controller.poll(focused));
            j.controller.feedback(j.engine.remotePlayFeedback(), focused);
        });
    }
    i.ps5ControlTimer->start(8);
    tickPs5Control(true);
#else
    (void)restart;
#endif
}
void QmlPlayerBridge::stopPs5Control() {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (!i.engine.remoteControlRequested()) return;
    veyra::log::info("remoteplay-control", "stop");
    i.engine.stopRemoteControl();
    i.ps5ControlProfile.clear();
    tickPs5Control(true);
#endif
}
void QmlPlayerBridge::tickPs5Control(bool force) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    auto& i = *impl_;
    if (!force && ++i.ps5ControlTicks % 15) return; // ~4 Hz from the 60 Hz poll
    // Switched on while the capture card was still opening, or the capture came
    // back by itself: start once it is live. A failed session stays requested
    // until the user restarts it, so this never loops.
    if (!force && !i.ps5ControlNoPair && i.prefBool("ps5CaptureControl", false) && captureCardLive() && !i.engine.remoteControlRequested())
        startPs5Control(false);
    const auto st = i.engine.remoteControlStatus();
    QString state = QStringLiteral("off");
    if (i.ps5ControlNoPair && !st.requested) state = QStringLiteral("nopair");
    else if (st.requested) {
        state = st.pinRequired ? QStringLiteral("pin")
              : st.recovering ? QStringLiteral("recovering")
              : st.alive && st.connected ? QStringLiteral("connected")
              : !st.alive && !st.starting ? QStringLiteral("failed")
              : QStringLiteral("starting");
    }
    // Tests only: show a state no console is available to produce here.
    if (const QString forced = qEnvironmentVariable("VEYRA_TEST_PS5_CONTROL_STATE"); !forced.isEmpty() && st.requested) state = forced;
    QVariantMap next;
    next[QStringLiteral("state")] = state;
    next[QStringLiteral("message")] = state == QLatin1String("nopair") ? tr("还没有配对的 PS5，先在「PS5 串流」里配对一次") : uiText(st.message);
    next[QStringLiteral("keepaliveFps")] = st.keepaliveReady ? std::round(st.keepaliveFps) : -1.0;
    const auto c = i.controller.capabilities();
    next[QStringLiteral("pad")] = (i.padUsers & Impl::PadPs5Control) && c.connected ? QString::fromStdString(c.name) : QString();
    next[QStringLiteral("gyro")] = c.gyro && c.accel;
    next[QStringLiteral("touch")] = c.touch;
    if (next == i.ps5ControlState) return;
    i.ps5ControlState = next;
    if (state != i.ps5ControlNoticed) {
        if (state == QLatin1String("connected")) emit notice(tr("PS5 手柄控制已连接"), false);
        else if (state == QLatin1String("pin")) emit notice(tr("PS5 要求输入登录 PIN：在采集卡面板「PS5 手柄控制」里填写"), false);
        else if (state == QLatin1String("failed")) {
            const QString reason = next.value(QStringLiteral("message")).toString();
            emit notice(reason.isEmpty() ? tr("PS5 手柄控制已停止") : reason, true);
        }
        if (state == QLatin1String("connected") || state == QLatin1String("pin") || state == QLatin1String("failed")) i.ps5ControlNoticed = state;
    }
    emit ps5ControlChanged();
#else
    (void)force;
#endif
}

// --- ps5 dialog --------------------------------------------------------------
QString QmlPlayerBridge::remotePlayHost() const { return impl_->remotePlayHost; }
void QmlPlayerBridge::setRemotePlayHost(const QString& value) {
    impl_->remotePlayHost = value;
    emit settingsChanged();
}
QString QmlPlayerBridge::remotePlayPin() const { return impl_->remotePlayPin; }
void QmlPlayerBridge::setRemotePlayPin(const QString& value) {
    // Held only until the connect command runs; it is never written to the session
    // file, and the engine keeps PSN credentials encrypted in the user data
    // directory rather than here.
    impl_->remotePlayPin = value;
    emit settingsChanged();
}



void QmlPlayerBridge::setGuest(guest::GuestStreamer* guest) {
#ifdef VEYRA_ENABLE_REMOTEPLAY
    impl_->ps5Input.setGuest(guest);
#else
    (void)guest;
#endif
}

} // namespace veyra::ui
