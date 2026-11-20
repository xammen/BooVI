include_guard(GLOBAL)
# Fork, partage: a remote guest watches and plays the stream from a browser (GuestStreamer.h).
#
#   veyra_guest   NVENC low-latency H.264 + Opus over WebRTC (libdatachannel, MPL-2.0), the
#                 share's room on BooVI's broker (BOOVI_BROKER_URL) or a small HTTP server in the
#                 tailnet, for the guest page and its signaling. No Qt.
#
# libdatachannel, nlohmann-json and Opus come from the same vcpkg tree as the Xbox build.
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)

veyra_add_compute_shader(veyra_shader_guest_nv12 GuestNv12)
add_library(veyra_guest STATIC
  "${VEYRA_ROOT}/include/veyra/guest/GuestProtocol.h"
  "${VEYRA_ROOT}/include/veyra/guest/GuestStreamer.h"
  "${VEYRA_ROOT}/include/veyra/guest/BrokerSocket.h"
  "${VEYRA_ROOT}/include/veyra/guest/GuestRoom.h"
  "${VEYRA_ROOT}/src/guest/GuestStreamer.cpp"
  "${VEYRA_ROOT}/src/guest/GuestVideo.cpp"
  "${VEYRA_ROOT}/src/guest/GuestVideo.h"
  "${VEYRA_ROOT}/src/guest/GuestAudio.cpp"
  "${VEYRA_ROOT}/src/guest/GuestAudio.h"
  "${VEYRA_ROOT}/src/guest/GuestHttp.cpp"
  "${VEYRA_ROOT}/src/guest/GuestHttp.h"
  "${VEYRA_ROOT}/src/guest/GuestBroker.cpp"
  "${VEYRA_ROOT}/src/guest/GuestBroker.h"
  "${VEYRA_ROOT}/src/guest/GuestCloudflare.cpp"
  "${VEYRA_ROOT}/src/guest/GuestCloudflare.h")
add_dependencies(veyra_guest veyra_shader_guest_nv12)
# The broker this build shares through, e.g. https://go.example.com; empty: tailnet only.
set(BOOVI_BROKER_URL "" CACHE STRING "BooVI's share broker (broker/), https://<host>; empty disables it")
target_compile_definitions(veyra_guest PRIVATE BOOVI_BROKER_URL="${BOOVI_BROKER_URL}")
target_compile_features(veyra_guest PUBLIC cxx_std_20)
target_compile_options(veyra_guest PRIVATE /utf-8)
target_compile_definitions(veyra_guest PUBLIC VEYRA_ENABLE_GUEST=1 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_include_directories(veyra_guest PUBLIC "${VEYRA_ROOT}/include")
target_link_libraries(veyra_guest
  PUBLIC veyra_engine
  PRIVATE LibDataChannel::LibDataChannel nlohmann_json::nlohmann_json Opus::opus ws2_32 iphlpapi bcrypt crypt32 winhttp d3d12 dxgi)
