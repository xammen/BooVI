# Installs the fork's development toolchain under %USERPROFILE%\dev without admin rights.
# Safe to rerun: every step is skipped when its output already exists.
#   - CMake, Ninja, aqtinstall (pip --user)
#   - Qt 6.8.3 msvc2022_64 (+ shadertools, multimedia)
#   - MSVC 2022 + Windows SDK, extracted by portable-msvc.py (accepts the VS Build Tools license)
#   - SDL 3.4.14 static, FFmpeg 9.0 dev files (BtbN LGPL build), DLSS SDK 310.7 headers and libs
# Not covered: nv-codec-headers (manual, see docs/DEVELOPMENT.md); chiaki-ng and its libraries
# come from build-rp-deps.ps1 and build.ps1.
param([string]$Dev = (Join-Path $env:USERPROFILE 'dev'))
$ErrorActionPreference = 'Stop'
$fork = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vsenv = Join-Path $PSScriptRoot 'vsenv.cmd'
New-Item -ItemType Directory -Force $Dev, "$Dev\deps\dl" | Out-Null
function Step($name, $done, [scriptblock]$run) {
    if (& $done) { Write-Host "ok    $name"; return }
    Write-Host "doing $name"; & $run
    if (-not (& $done)) { throw "$name did not complete" }
}

Step 'cmake ninja aqtinstall' { (py -m pip show cmake ninja aqtinstall 2>$null | Select-String '^Name:').Count -eq 3 } {
    py -m pip install --user --quiet cmake ninja aqtinstall }
$pyScripts = (py -c "import sysconfig; print(sysconfig.get_path('scripts', 'nt_user'))").Trim()
$env:PATH = "$pyScripts;$env:PATH"

Step 'Qt 6.8.3' { Test-Path "$Dev\Qt\6.8.3\msvc2022_64\bin\qmllint.exe" } {
    py -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O "$Dev\Qt" -m qtshadertools qtmultimedia }

Step 'MSVC 2022 (portable)' { Test-Path "$Dev\toolchain\msvc\setup_x64.bat" } {
    New-Item -ItemType Directory -Force "$Dev\dl-msvc", "$Dev\toolchain" | Out-Null
    Invoke-WebRequest 'https://gist.githubusercontent.com/mmozeiko/7f3162ec2988e81e56d5c4e22cde9977/raw/portable-msvc.py' -OutFile "$Dev\dl-msvc\portable-msvc.py"
    Push-Location "$Dev\toolchain"; try { py -I "$Dev\dl-msvc\portable-msvc.py" --vs 2022 --accept-license } finally { Pop-Location } }
$env:VEYRA_MSVC = "$Dev\toolchain\msvc"

Step 'SDL 3.4.14' { Test-Path "$Dev\deps\sdl3\lib\SDL3-static.lib" } {
    if (-not (Test-Path "$Dev\SDL")) { git clone -q --depth 1 --branch release-3.4.14 https://github.com/libsdl-org/SDL.git "$Dev\SDL" }
    cmd /c "`"$vsenv`" cmake -S `"$Dev\SDL`" -B `"$Dev\SDL\build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_INSTALL_PREFIX=`"$Dev\deps\sdl3`""
    cmd /c "`"$vsenv`" cmake --build `"$Dev\SDL\build`" --target install" }

Step 'FFmpeg 9.0 dev files' { Test-Path "$Dev\deps\ffmpeg\*\include\libavformat\avformat.h" } {
    $zip = "$Dev\deps\dl\ffmpeg9.zip"
    Invoke-WebRequest 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip' -OutFile $zip
    Expand-Archive $zip -DestinationPath "$Dev\deps\ffmpeg" -Force }

Step 'FidelityFX SDK 2.3.0 headers' { Test-Path "$Dev\deps\ffx2\Kits\FidelityFX\framegeneration\include\dx12\ffx_api_framegeneration_dx12.h" } {
    git clone -q --depth 1 --branch v2.3.0 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git "$Dev\deps\ffx2"
    git -C "$Dev\deps\ffx2" sparse-checkout set Kits/FidelityFX/api/include Kits/FidelityFX/framegeneration/include Kits/FidelityFX/upscalers/include }

# The optical flow that frame generation uses when NVIDIA NVOF is not in the build. Only the
# FidelityFX SDK 1.1.4 static libraries have it; four local patches let them build with Ninja
# and without the PIX runtime.
Step 'FidelityFX SDK 1.1.4 optical flow' { Test-Path "$Dev\deps\ffx1\sdk\bin\ffx_sdk\ffx_opticalflow_x64.lib" } {
    $sdk = "$Dev\deps\ffx1\sdk"
    if (-not (Test-Path $sdk)) {
        git clone -q --depth 1 --branch v1.1.4 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git "$Dev\deps\ffx1"
        git -C "$Dev\deps\ffx1" sparse-checkout set sdk
    }
    function Patch($file, $old, $new) {
        $text = [IO.File]::ReadAllText($file)
        if (-not $text.Contains($new)) { [IO.File]::WriteAllText($file, $text.Replace($old, $new)) }
    }
    Patch "$sdk\toolchain.cmake" "`tset(CMAKE_GENERATOR_PLATFORM `"x64`" CACHE STRING `"`" FORCE)" `
        "`tif(NOT CMAKE_GENERATOR MATCHES `"Ninja`")`n`t`tset(CMAKE_GENERATOR_PLATFORM `"x64`" CACHE STRING `"`" FORCE)`n`tendif()`n`tset(CMAKE_EXE_LINKER_FLAGS `"/machine:x64`")"
    Patch "$sdk\src\backends\dx12\ffx_dx12.cpp" "#define USE_PIX  //" "// fork build: no PIX runtime //"
    Patch "$sdk\include\FidelityFX\gpu\CMakeCompileShaders.txt" 'list(APPEND PERMUTATION_OUTPUTS ' 'list(APPEND _FFX_OUTS '
    Patch "$sdk\include\FidelityFX\gpu\CMakeCompileShaders.txt" 'set(${PERMUTATION_OUTPUTS} ${PERMUTATION_OUTPUTS} PARENT_SCOPE)' 'set(${PERMUTATION_OUTPUTS} ${_FFX_OUTS} PARENT_SCOPE)'
    [IO.File]::WriteAllText("$sdk\libs\pix\CMakeLists.txt", "add_library(pixlib INTERFACE)`ntarget_include_directories(pixlib INTERFACE `${CMAKE_CURRENT_SOURCE_DIR})`n")
    $off = 'FSR1 FSR2 SPD CACAO LPM BLUR CAS SSSR VRS PARALLEL_SORT DOF LENS CLASSIFIER BRIXELIZER BRIXELIZER_GI BREADCRUMBS FSR3UPSCALER FI DENOISER'.Split(' ') | ForEach-Object { "-DFFX_$_=OFF" }
    cmd /c "`"$vsenv`" cmake -S `"$sdk`" -B `"$sdk\build`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFX_API_BACKEND=DX12_X64 -DFFX_FSR3=ON -DFFX_OF=ON $($off -join ' ') -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded 2>&1"
    cmd /c "`"$vsenv`" cmake --build `"$sdk\build`" --target ffx_opticalflow_x64 ffx_backend_dx12_x64 2>&1" }

Step 'DLSS SDK 310.7'{ Test-Path "$Dev\deps\dlss\include\nvsdk_ngx.h" } {
    git clone -q --depth 1 --branch v310.7.0 --filter=blob:none --sparse https://github.com/NVIDIA/DLSS.git "$Dev\deps\dlss"
    git -C "$Dev\deps\dlss" sparse-checkout set include lib/Windows_x86_64/x64 }

Write-Host "done. Fork tests: scripts\fork\run-tests.cmd (from $fork)"
