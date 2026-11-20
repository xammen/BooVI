#include "GuestTunnel.h"

#include <windows.h>

#include "veyra/Log.h"
#include "veyra/guest/CloudflaredLog.h"

#include <filesystem>
#include <format>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace veyra::guest {
namespace {

std::wstring cloudflaredExe() {
    wchar_t found[MAX_PATH]{};
    if (SearchPathW(nullptr, L"cloudflared.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    for (const wchar_t* var : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        wchar_t dir[MAX_PATH]{};
        if (!GetEnvironmentVariableW(var, dir, MAX_PATH)) continue;
        const auto exe = std::filesystem::path(dir) / L"cloudflared" / L"cloudflared.exe";
        std::error_code ec;
        if (std::filesystem::exists(exe, ec)) return exe.wstring();
    }
    return {};
}

// This process's environment with `name` set to `value`, as CreateProcessW takes it.
std::vector<wchar_t> environmentWith(const std::wstring& name, const std::wstring& value) {
    std::vector<wchar_t> block;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* s = env; *s; s += wcslen(s) + 1) {
            const size_t length = wcslen(s);
            if (!name.empty() && length > name.size() && _wcsnicmp(s, name.c_str(), name.size()) == 0 && s[name.size()] == L'=') continue;
            block.insert(block.end(), s, s + length + 1);
        }
        FreeEnvironmentStringsW(env);
    }
    if (!name.empty()) {
        block.insert(block.end(), name.begin(), name.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(0);
    }
    block.push_back(0);
    return block;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    std::wstring out(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), int(out.size()));
    return out;
}

} // namespace

struct GuestTunnel::Impl {
    mutable std::mutex m;
    TunnelPhase phase = TunnelPhase::Off;
    bool quick = false;
    std::string url, error;
    std::set<int> up;   // cloudflared's registered connections, by connIndex
    HANDLE job = nullptr, process = nullptr, output = nullptr;
    std::thread reader;

    bool launch(const std::wstring& args, const std::wstring& envName, const std::wstring& envValue, std::string& err) {
        const std::wstring exe = cloudflaredExe();
        if (exe.empty()) { err = "cloudflared introuvable"; return false; }
        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) { err = "pipe"; return false; }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
        // Only the output pipe and NUL reach the child, none of BooVI's other handles.
        HANDLE inherit[2] = {writeEnd, nul};
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<uint8_t> attrBuffer(size);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
        InitializeProcThreadAttributeList(attrs, 1, 0, &size);
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul;
        si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = writeEnd;
        si.lpAttributeList = attrs;

        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        std::wstring cmd = L"\"" + exe + L"\" " + args;
        auto env = environmentWith(envName, envValue);
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            env.data(), nullptr, &si.StartupInfo, &pi);
        const DWORD code = GetLastError();
        SecureZeroMemory(env.data(), env.size() * sizeof(wchar_t));
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(writeEnd);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) { err = std::format("cloudflared ne démarre pas ({})", code); CloseHandle(readEnd); closeHandles(); return false; }
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(readEnd); closeHandles();
            err = "job object";
            return false;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        output = readEnd;
        reader = std::thread([this] { readLoop(); });
        return true;
    }

    void readLoop() {
        SetThreadDescription(GetCurrentThread(), L"Guest tunnel");
        std::string pending;
        char buffer[4096];
        DWORD n = 0;
        while (ReadFile(output, buffer, sizeof(buffer), &n, nullptr) && n) {
            pending.append(buffer, n);
            for (size_t nl; (nl = pending.find('\n')) != std::string::npos; pending.erase(0, nl + 1)) onLine(std::string_view(pending).substr(0, nl));
            if (pending.size() > 64 * 1024) pending.clear();
        }
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        std::lock_guard lock(m);
        if (phase == TunnelPhase::Off) return;
        phase = TunnelPhase::Failed;
        up.clear();
        error = std::format("cloudflared s'est arrêté (code {})", code);
        veyra::log::warn("guest-tunnel", error);
    }

    // The line itself is never logged: only what BooVI takes from it.
    void onLine(std::string_view line) {
        const auto parsed = parseCloudflaredLine(line);
        std::lock_guard lock(m);
        if (phase == TunnelPhase::Off) return;
        if (quick && url.empty() && !parsed.quickUrl.empty()) {
            url = parsed.quickUrl;
            veyra::log::info("guest-tunnel", "quick tunnel address received");
        }
        // cloudflared retries a connection that never came up: only a change counts.
        const bool changed = parsed.event == TunnelEvent::Up ? up.insert(parsed.connIndex).second
                           : parsed.event == TunnelEvent::Down && up.erase(parsed.connIndex) > 0;
        if (!changed) return;
        const TunnelPhase next = !up.empty() && !url.empty() ? TunnelPhase::Connected : TunnelPhase::Starting;
        veyra::log::info("guest-tunnel", std::format("connection {} {} ({} open)", parsed.connIndex,
            parsed.event == TunnelEvent::Up ? "up" : "down", up.size()));
        phase = next;
    }

    void closeHandles() {
        if (process) { CloseHandle(process); process = nullptr; }
        if (output) { CloseHandle(output); output = nullptr; }
        if (job) { CloseHandle(job); job = nullptr; }
    }
};

GuestTunnel::GuestTunnel() : p_(std::make_shared<Impl>()) {}
GuestTunnel::~GuestTunnel() { stop(); }

bool GuestTunnel::startNamed(const std::string& token, const std::string& hostname, std::string& error) {
    stop();
    { std::lock_guard lock(p_->m); p_->phase = TunnelPhase::Starting; p_->quick = false; p_->url = "https://" + hostname; p_->error.clear(); }
    std::wstring secret = widen(token);
    const bool ok = p_->launch(L"tunnel --no-autoupdate --loglevel info run", L"TUNNEL_TOKEN", secret, error);
    SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
    if (!ok) { std::lock_guard lock(p_->m); p_->phase = TunnelPhase::Failed; p_->error = error; return false; }
    veyra::log::info("guest-tunnel", "named tunnel starting");
    return true;
}

bool GuestTunnel::startQuick(uint16_t port, std::string& error) {
    stop();
    { std::lock_guard lock(p_->m); p_->phase = TunnelPhase::Starting; p_->quick = true; p_->url.clear(); p_->error.clear(); }
    const auto args = std::format(L"tunnel --no-autoupdate --loglevel info --url http://127.0.0.1:{}", port);
    if (!p_->launch(args, L"", L"", error)) { std::lock_guard lock(p_->m); p_->phase = TunnelPhase::Failed; p_->error = error; return false; }
    veyra::log::info("guest-tunnel", std::format("quick tunnel starting for port {}", port));
    return true;
}

void GuestTunnel::stop() {
    auto& p = *p_;
    {
        std::lock_guard lock(p.m);
        if (p.phase == TunnelPhase::Off && !p.job) return;
        p.phase = TunnelPhase::Off;
        p.url.clear(); p.up.clear();
    }
    if (p.job) TerminateJobObject(p.job, 0);
    if (p.reader.joinable()) p.reader.join();
    p.closeHandles();
    veyra::log::info("guest-tunnel", "tunnel stopped");
}

TunnelPhase GuestTunnel::phase() const { std::lock_guard lock(p_->m); return p_->phase; }
std::string GuestTunnel::publicUrl() const { std::lock_guard lock(p_->m); return p_->url; }
std::string GuestTunnel::error() const { std::lock_guard lock(p_->m); return p_->error; }

} // namespace veyra::guest
