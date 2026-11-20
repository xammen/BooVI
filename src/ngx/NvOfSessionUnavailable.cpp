// Fork: NvOfSession when the NVIDIA Optical Flow SDK is not available at build time. The SDK's
// D3D12 header is only in the login-gated download, so a build without it still links: every
// session reports that optical flow cannot load, exactly as when nvofapi64.dll is missing,
// and the pipeline falls back to its own motion estimation.
#include "veyra/ngx/NvOfSession.h"
#include "veyra/Log.h"

namespace veyra::ngx {

struct NvOfSession::FnList {};

bool NvOfSession::sdkAvailable() { return false; }

NvOfSession::~NvOfSession() { shutdown(); }

bool NvOfSession::initialize(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                             const Desc&, Status& status) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        log::warn("nvof-session", "built without the NVIDIA Optical Flow SDK; optical flow unavailable");
    }
    status = Status::LoadLibraryFailure;
    return false;
}

bool NvOfSession::execute(uint64_t, Status& status) {
    status = Status::InvalidArgument;
    return false;
}

bool NvOfSession::unregisterAll(Status& status) {
    status = Status::Ok;
    return true;
}

void NvOfSession::shutdown() { initialized_ = false; }

}
