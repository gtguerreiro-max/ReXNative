# Builds the ReXGlue SDK ReXNative's renderer needs: ReXGlue v0.11.0 (sdk/rexglue.lock) with
# sdk/patches/v0.11/ applied in order, configured with its win-amd64 preset and built with LLVM 20
# (clang++, lld) in the Visual Studio environment, installed for a game's CMake. Also checks out
# the AMD FidelityFX SDK at sdk/fidelityfx.lock (the renderer's FSR / frame generation headers).
#
#   scripts\build-sdk.ps1 [-Dest build] [-SdkSrc <dir>] [-Jobs 10] [-Llvm <LLVM 20 bin>]
#                         [-VulkanInclude <dir> -VulkanLib <vulkan-1.lib> -VmaInclude <dir>]
#                         [-FidelityFx <dir>] [-NoBuild]
#
#   -Dest           build tree: <Dest>\rexglue-sdk (source, unless -SdkSrc), <Dest>\sdk (build),
#                   <Dest>\sdk-install (install), <Dest>\fidelityfx-sdk, <Dest>\logs
#   -SdkSrc         an existing ReXGlue checkout to use (patched already, or made here when it
#                   has no .git yet)
#   -Llvm           LLVM 20's bin folder (default $env:REXNATIVE_LLVM, then
#                   C:\Program Files\LLVM\bin): ReXGlue v0.11 needs clang 20 or later
#   -VulkanInclude, -VulkanLib, -VmaInclude
#                   the Vulkan headers, vulkan-1.lib and the Vulkan Memory Allocator header, when
#                   the Vulkan SDK ($env:VULKAN_SDK) is not installed (ReXGlue builds its Vulkan
#                   driver too)
#   -FidelityFx     the FidelityFX SDK v2.4.0 checkout (made here when absent)
#   -NoBuild        fetch and patch only
#
# Needs git, Python 3, CMake, Ninja, LLVM 20 and Visual Studio 2022 (or its Build Tools) with the
# C++ workload (the Windows SDK and the MSVC libraries). The NVIDIA DLSS SDK and DLSS 5 headers
# are not fetched: get them from NVIDIA and pass their paths to rexnative_add().
param(
    [string]$Dest = "$PSScriptRoot\..\build",
    [string]$SdkSrc = '',
    [string]$FidelityFx = '',
    [string]$Llvm = $env:REXNATIVE_LLVM,
    [string]$VulkanInclude = '',
    [string]$VulkanLib = '',
    [string]$VmaInclude = '',
    [int]$Jobs = 10,
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$Dest = [IO.Path]::GetFullPath($Dest)
if (-not $SdkSrc) { $SdkSrc = "$Dest\rexglue-sdk" }
if (-not $FidelityFx) { $FidelityFx = "$Dest\fidelityfx-sdk" }
if (-not $Llvm) { $Llvm = 'C:\Program Files\LLVM\bin' }
$vc = "$PSScriptRoot\vcenv.cmd"
New-Item -ItemType Directory -Force "$Dest\logs" | Out-Null

function Lock([string]$path) {
    $lock = @{}
    Get-Content $path | Where-Object { $_ -match '^(\w+)=(.*)$' } | ForEach-Object { $lock[$Matches[1]] = $Matches[2] }
    return $lock
}
function Run([string]$cmd, [string]$log) {
    # cmd's redirection: Windows PowerShell 5.1 never sees native stderr (with
    # ErrorActionPreference Stop it would turn vcvars' harmless warnings into a failure).
    cmd /c "call `"$vc`" $cmd > `"$Dest\logs\$log`" 2>&1"
    if ($LASTEXITCODE) { throw "$log failed (see $Dest\logs\$log)" }
}

# -- ReXGlue at the lock, patched ---------------------------------------------------------------
if (-not (Test-Path "$SdkSrc\.git")) {
    $lock = Lock "$repo\sdk\rexglue.lock"
    # No CRLF conversion (the patches expect the files as committed), and libmspack's git symlinks,
    # which become text placeholders without Windows developer mode, are replaced by copies of
    # their targets.
    git clone -q --no-checkout --filter=blob:none $lock.REXGLUE_URL $SdkSrc
    if ($LASTEXITCODE) { throw 'git clone of ReXGlue failed' }
    git -C $SdkSrc config core.autocrlf false
    git -C $SdkSrc checkout -q $lock.REXGLUE_COMMIT
    if ($LASTEXITCODE) { throw "ReXGlue commit $($lock.REXGLUE_COMMIT) not found" }
    git -C $SdkSrc submodule update -q --init --recursive --depth 1 --jobs 8
    git -C $SdkSrc submodule foreach -q --recursive 'git config core.autocrlf false'
    python -c @"
import subprocess, os, shutil
repo = r'$SdkSrc\thirdparty\libmspack'
if os.path.isdir(repo):
    for line in subprocess.run(['git','-C',repo,'ls-files','-s'],capture_output=True,text=True).stdout.splitlines():
        mode, _, _, path = line.split(None, 3)
        if mode == '120000':
            p = os.path.join(repo, path)
            shutil.copyfile(os.path.normpath(os.path.join(os.path.dirname(p), open(p).read().strip())), p)
"@
    $patches = @(Get-ChildItem "$repo\sdk\patches\v0.11\*.patch" | Sort-Object Name)
    foreach ($p in $patches) {
        git -C $SdkSrc apply --whitespace=nowarn $p.FullName
        if ($LASTEXITCODE) { throw "patch $($p.Name) does not apply" }
    }
    "ReXGlue $($lock.REXGLUE_COMMIT) + $($patches.Count) patches in $SdkSrc"
} else {
    "Using the ReXGlue checkout in $SdkSrc as it is"
}

# -- FidelityFX SDK (the renderer's headers) ------------------------------------------------------
if (-not (Test-Path "$FidelityFx\Kits")) {
    $ffx = Lock "$repo\sdk\fidelityfx.lock"
    git clone -q --no-checkout --filter=blob:none $ffx.FFX_URL $FidelityFx
    if ($LASTEXITCODE) { throw 'git clone of the FidelityFX SDK failed' }
    git -C $FidelityFx checkout -q $ffx.FFX_COMMIT
    if ($LASTEXITCODE) { throw "FidelityFX commit $($ffx.FFX_COMMIT) not found" }
}

# -- Build and install ------------------------------------------------------------------------------
if (-not $NoBuild) {
    if (-not (Test-Path "$Llvm\clang++.exe")) { throw "LLVM 20 not found in $Llvm (pass -Llvm)" }
    $vulkan = ''
    if ($VulkanInclude) { $vulkan += " `"-DVulkan_INCLUDE_DIR=$VulkanInclude`"" }
    if ($VulkanLib) { $vulkan += " `"-DVulkan_LIBRARY=$VulkanLib`"" }
    if ($VmaInclude) { $vulkan += " `"-DREXGLUE_VMA_INCLUDE_DIR=$VmaInclude`"" }
    $path = "set `"PATH=$Llvm;%PATH%`""
    Run "$path && cd /d `"$SdkSrc`" && cmake --preset win-amd64 -B `"$Dest\sdk`" -DREXGLUE_BUILD_TESTS=OFF$vulkan `"-DCMAKE_INSTALL_PREFIX=$Dest\sdk-install`"" 'sdk-configure.log'
    Run "$path && cmake --build `"$Dest\sdk`" --config Release --target install -j $Jobs" 'sdk-build.log'
}

''
'For the game (CMake, clang++ from LLVM 20):'
"  -DCMAKE_PREFIX_PATH=$Dest\sdk-install"
'  rexnative_add(<target>'
"      REXGLUE_SDK_SRC $SdkSrc"
"      FIDELITYFX_SDK  $FidelityFx"
'      DLSS_SDK        <NVIDIA DLSS SDK>   DLSS_NR <DLSS 5 headers>'
'      LAYER_DIR       <the game''s layer: python -m rexnative adapt>)'
