# Builds the native libraries chiaki-ng needs for PS5 Remote Play, with the portable MSVC
# and CMake (no vcpkg: it requires an installed Visual Studio). Static libraries and static
# CRT (/MT), like the Veyra product build (CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded).
# Output prefix: %USERPROFILE%\dev\deps\rp (include, lib, lib\pkgconfig). Safe to rerun.
# OpenSSL comes prebuilt from Qt's tool repository (aqt), pkg-config from the pkgconf wheel.
param([string]$Dev = (Join-Path $env:USERPROFILE 'dev'))
$ErrorActionPreference = 'Stop'
$vsenv = Join-Path $PSScriptRoot 'vsenv.cmd'
$prefix = "$Dev\deps\rp"
$src = "$Dev\deps\rp-src"
New-Item -ItemType Directory -Force $prefix, $src | Out-Null
$pyScripts = (py -c "import sysconfig; print(sysconfig.get_path('scripts', 'nt_user'))").Trim()
$env:PATH = "$pyScripts;$env:PATH"

function Build([string]$name, [string]$repo, [string]$tag, [string]$marker, [string]$sub = '', [string[]]$opts = @()) {
    if (Test-Path "$prefix\$marker") { Write-Host "ok    $name"; return }
    Write-Host "doing $name $tag"
    $dir = "$src\$name"
    if (-not (Test-Path $dir)) { git clone -q --depth 1 --branch $tag $repo $dir; if ($LASTEXITCODE) { throw "clone $name" } }
    $s = if ($sub) { "$dir\$sub" } else { $dir }
    $common = "-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=`"$prefix`" -DBUILD_SHARED_LIBS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
    cmd /c "`"$vsenv`" cmake -S `"$s`" -B `"$dir\build`" $common $($opts -join ' ')" | Out-Host
    if ($LASTEXITCODE) { throw "configure $name" }
    cmd /c "`"$vsenv`" cmake --build `"$dir\build`" --target install" | Out-Host
    if ($LASTEXITCODE) { throw "build $name" }
    if (-not (Test-Path "$prefix\$marker")) { throw "$name installed no $marker" }
}

Build 'opus' 'https://github.com/xiph/opus.git' 'v1.6.1' 'lib\opus.lib' '' @('-DOPUS_BUILD_TESTING=OFF', '-DOPUS_BUILD_PROGRAMS=OFF')
Build 'json-c' 'https://github.com/json-c/json-c.git' 'json-c-0.18-20240915' 'lib\pkgconfig\json-c.pc' '' @('-DBUILD_TESTING=OFF', '-DBUILD_APPS=OFF', '-DDISABLE_WERROR=ON')
Build 'miniupnpc' 'https://github.com/miniupnp/miniupnp.git' 'miniupnpc_2_3_5' 'lib\pkgconfig\miniupnpc.pc' 'miniupnpc' @('-DUPNPC_BUILD_SHARED=OFF', '-DUPNPC_BUILD_STATIC=ON', '-DUPNPC_BUILD_TESTS=OFF', '-DUPNPC_BUILD_SAMPLE=OFF')
Build 'libevent' 'https://github.com/libevent/libevent.git' 'release-2.1.12-stable' 'lib\event.lib' '' @('-DEVENT__DISABLE_OPENSSL=ON', '-DEVENT__DISABLE_MBEDTLS=ON', '-DEVENT__DISABLE_TESTS=ON', '-DEVENT__DISABLE_SAMPLES=ON', '-DEVENT__DISABLE_BENCHMARK=ON', '-DEVENT__LIBRARY_TYPE=STATIC', '-DEVENT__MSVC_STATIC_RUNTIME=ON')

if (-not (Test-Path "$Dev\Qt\Tools\OpenSSLv3\Win_x64\include\openssl\ssl.h")) {
    Write-Host 'doing OpenSSL 3 (Qt tools)'
    py -m aqt install-tool windows desktop tools_opensslv3_x64 -O "$Dev\Qt"
}
$ssl = "$Dev\Qt\Tools\OpenSSLv3\Win_x64"

# PC streaming (Moonlight): the pinned moonlight-common-c checkout, built by Veyra's own CMake.
$ml = "$Dev\deps\moonlight-common-c"
if (-not (Test-Path "$ml\src\Limelight.h")) {
    Write-Host 'doing moonlight-common-c'
    git clone -q https://github.com/moonlight-stream/moonlight-common-c.git $ml
    git -C $ml checkout -q f900dd4767759c7b9d0e93bcea666b55c69ea62f
    git -C $ml submodule update --init --recursive -q
} else { Write-Host 'ok    moonlight-common-c' }

# Xbox streaming: nlohmann_json and libdatachannel 0.24.5 (bundled libjuice, usrsctp, libsrtp, plog)
# with Veyra's libjuice IPv4 interrupt-socket patch (scripts/xbox/vcpkg-overlay/libjuice).
Build 'nlohmann_json' 'https://github.com/nlohmann/json.git' 'v3.11.3' 'share\cmake\nlohmann_json\nlohmann_jsonConfig.cmake' '' @('-DJSON_BuildTests=OFF', '-DJSON_Install=ON')
$ldc = "$src\libdatachannel"
if (-not (Test-Path "$prefix\lib\datachannel.lib") -and -not (Test-Path $ldc)) {
    git clone -q --depth 1 --branch v0.24.5 --recurse-submodules --shallow-submodules https://github.com/paullouisageneau/libdatachannel.git $ldc
    git -C "$ldc\deps\libjuice" apply "$(Split-Path -Parent (Split-Path -Parent $PSScriptRoot))\scripts\xbox\vcpkg-overlay\libjuice\ipv4-interrupt.diff"
    if ($LASTEXITCODE) { throw 'libjuice patch' }
}
Build 'libdatachannel' 'https://github.com/paullouisageneau/libdatachannel.git' 'v0.24.5' 'lib\datachannel.lib' '' @('-DNO_EXAMPLES=ON', '-DNO_TESTS=ON', '-DUSE_GNUTLS=OFF', '-DUSE_MBEDTLS=OFF', '-DUSE_NICE=OFF', "-DOPENSSL_ROOT_DIR=`"$ssl`"", '-DOPENSSL_USE_STATIC_LIBS=OFF')

if (-not (Get-Command pkgconf -ErrorAction SilentlyContinue)) { py -m pip install --user --quiet pkgconf }
Write-Host "done: $prefix"
