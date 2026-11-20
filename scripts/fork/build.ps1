# Configures and builds the fork (QML app with PS5 Remote Play, PC and Xbox streaming) with the toolchain from
# setup-dev.ps1 and the libraries from build-rp-deps.ps1, then stages a runnable app folder.
# Usage: scripts\fork\build.ps1 -Base <Veyra-2.1.9 portable dir> [-Out <app dir>] [-Repo <owner/name>]
# -Base supplies the prebuilt runtime (Qt, NVIDIA, AMD and Intel DLLs): see docs/DEVELOPMENT.md.
# -Repo is BooVI's GitHub repository, for update checks; without it the build checks nothing.
param([Parameter(Mandatory)][string]$Base,
      [string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'out\app'),
      [string]$Dev = (Join-Path $env:USERPROFILE 'dev'),
      [string]$Repo = '')
$ErrorActionPreference = 'Stop'
$fork = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vsenv = Join-Path $PSScriptRoot 'vsenv.cmd'
$pyScripts = (py -c "import sysconfig; print(sysconfig.get_path('scripts', 'nt_user'))").Trim()
$env:PATH = "$Dev\deps\protoc\bin;$pyScripts;$env:PATH"   # nanopb's generator calls protoc from PATH
if (-not (Test-Path "$Dev\deps\protoc\bin\protoc.exe")) {
    New-Item -ItemType Directory -Force "$Dev\deps\protoc" | Out-Null
    Invoke-WebRequest 'https://github.com/protocolbuffers/protobuf/releases/download/v36.2/protoc-36.2-win64.zip' -OutFile "$Dev\deps\protoc\p.zip"
    Expand-Archive "$Dev\deps\protoc\p.zip" -DestinationPath "$Dev\deps\protoc" -Force
}
if (-not (Test-Path "$Dev\chiaki-stage\lib\include\chiaki\session.h")) {
    $pin = '0e16950165f06e5c3291537c2eeba6e852be7120'
    if (-not (Test-Path "$Dev\chiaki-ng")) { git clone -q --filter=blob:none https://github.com/streetpea/chiaki-ng.git "$Dev\chiaki-ng" }
    git -C "$Dev\chiaki-ng" checkout -q $pin
    git -C "$Dev\chiaki-ng" submodule update --init --recursive -q
    git -C "$Dev\chiaki-ng" worktree add -q --detach "$Dev\chiaki-stage" $pin
    git -C "$Dev\chiaki-stage" submodule update --init --recursive -q
    foreach ($p in '0001-chiaki-msvc-vla-compat.patch', '0002-chiaki-video-metadata.patch') {
        git -C "$Dev\chiaki-stage" apply "$fork\scripts\remoteplay\patches\$p"; if ($LASTEXITCODE) { throw "patch $p" }
    }
}
# NVIDIA VFG binds the CUDA driver API at run time; it only needs cuda.h to compile.
# The header comes from NVIDIA's public CUDA redist archive, nothing from it is staged.
$cuda = "$Dev\deps\cuda-cudart-redist\cuda_cudart-windows-x86_64-12.8.57-archive\include"
if (-not (Test-Path "$cuda\cuda.h")) {
    New-Item -ItemType Directory -Force "$Dev\deps\cuda-cudart-redist" | Out-Null
    $zip = "$Dev\deps\cuda-cudart-redist\cudart.zip"
    Invoke-WebRequest 'https://developer.download.nvidia.com/compute/cuda/redist/cuda_cudart/windows-x86_64/cuda_cudart-windows-x86_64-12.8.57-archive.zip' -OutFile $zip
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne '2C7AA62A195D79229D4381C8BD0174A30502CF3D8124C6E94EE50A7FC8A1E9F4') { throw 'cudart archive hash mismatch' }
    Expand-Archive $zip -DestinationPath "$Dev\deps\cuda-cudart-redist" -Force
}
$ff = (Get-ChildItem "$Dev\deps\ffmpeg" -Directory | Select-Object -First 1).FullName
$defs = @(
    '-DVEYRA_BUILD_QML_UI=ON',
    "-DCMAKE_PREFIX_PATH=`"$Dev\Qt\6.8.3\msvc2022_64;$Dev\deps\rp;$Dev\deps\sdl3`"",
    "-DVEYRA_FFMPEG_ROOT=`"$ff`"",
    "-DVEYRA_FIDELITYFX2_ROOT=`"$Dev\deps\ffx2\Kits\FidelityFX`"",
    "-DVEYRA_FIDELITYFX_ROOT=`"$Dev\deps\ffx1\sdk`"",
    '-DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON', "-DVEYRA_DLSS_SDK_ROOT=`"$Dev\deps\dlss`"",
    "-DVEYRA_NVENC_HEADERS_ROOT=`"$Dev\deps\nv-codec-headers-13.0`"",
    "-DVEYRA_CUDA_DRIVER_INCLUDE_DIR=`"$cuda`"",
    '-DVEYRA_ENABLE_REMOTEPLAY=ON',
    '-DVEYRA_ENABLE_MOONLIGHT=ON', "-DVEYRA_MOONLIGHT_SOURCE_DIR=`"$Dev\deps\moonlight-common-c`"",
    '-DVEYRA_ENABLE_XBOX=ON',
    "-DVEYRA_RP_CHIAKI_SOURCE_DIR=`"$Dev\chiaki-stage`"", "-DVEYRA_RP_CHIAKI_VERIFY_DIR=`"$Dev\chiaki-ng`"",
    "-DPROTOC=`"$Dev\deps\protoc\bin\protoc.exe`"", "-DPKG_CONFIG_EXECUTABLE=`"$pyScripts\pkgconf.exe`"",
    "-DOPENSSL_ROOT_DIR=`"$Dev\Qt\Tools\OpenSSLv3\Win_x64`"",
    "-DBOOVI_GITHUB_REPO=$Repo")
Push-Location $fork
try {
    cmd /c "`"$vsenv`" cmake --preset x64-release -B out\build\fork $($defs -join ' ') 2>&1"
    if ($LASTEXITCODE) { throw 'configure failed' }
    cmd /c "`"$vsenv`" cmake --build out\build\fork --target veyra_qml_ui 2>&1"
    if ($LASTEXITCODE) { throw 'build failed' }
} finally { Pop-Location }
& (Join-Path $PSScriptRoot 'stage-app.ps1') -Base $Base -Out $Out -Dev $Dev
