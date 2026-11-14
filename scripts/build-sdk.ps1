# Builds the ReXGlue SDK ReXNative's renderer needs: upstream ReXGlue at sdk/rexglue.lock with
# sdk/patches/pgr4native/ then sdk/patches/rexnative/ applied in order, configured and built with
# Visual Studio 2022's clang-cl and lld-link, installed for a game's CMake. Also checks out the
# AMD FidelityFX SDK at sdk/fidelityfx.lock (the renderer's FSR / frame generation headers).
# From FromDustDeck's scripts/build.ps1 (its sdk stage), which Iron Brigade's port reuses.
#
#   scripts\build-sdk.ps1 [-Dest build] [-SdkSrc <dir>] [-Jobs 10] [-Tune generic]
#                         [-FidelityFx <dir>] [-FidelityFxSrc <dir>] [-NoBuild]
#
#   -Dest           build tree: <Dest>\rexglue-sdk (source, unless -SdkSrc), <Dest>\sdk (build),
#                   <Dest>\sdk-install (install), <Dest>\fidelityfx-sdk, <Dest>\logs
#   -SdkSrc         an existing ReXGlue checkout to use (patched already, or made here when it
#                   has no .git yet)
#   -FidelityFx     the FidelityFX SDK v2.3.0 checkout (made here when absent)
#   -FidelityFxSrc  a local copy of the FidelityFX tree the SDK's CMake fetches (rexglue's fork,
#                   cmake/rexglue_fidelityfx.cmake), to build without fetching it
#   -Tune           -mtune (znver2 for the Steam Deck); x86-64-v3 either way
#   -NoBuild        fetch and patch only
#
# Needs git, Python 3, CMake, Ninja and Visual Studio 2022 (or its Build Tools) with the C++
# workload and "C++ Clang tools for Windows". The NVIDIA DLSS SDK and DLSS 5 headers are not
# fetched: get them from NVIDIA and pass their paths to rexnative_add().
param(
    [string]$Dest = "$PSScriptRoot\..\build",
    [string]$SdkSrc = '',
    [string]$FidelityFx = '',
    [string]$FidelityFxSrc = '',
    [string]$Tune = 'generic',
    [int]$Jobs = 10,
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$Dest = [IO.Path]::GetFullPath($Dest)
if (-not $SdkSrc) { $SdkSrc = "$Dest\rexglue-sdk" }
if (-not $FidelityFx) { $FidelityFx = "$Dest\fidelityfx-sdk" }
$vc = "$PSScriptRoot\vcenv.cmd"
$flags = "/clang:-march=x86-64-v3 /clang:-mtune=$Tune /clang:-mprefer-vector-width=128 /clang:-ffp-model=precise /clang:-fno-math-errno /Zc:char8_t- -Wno-unknown-argument -Wno-unused-command-line-argument"
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
    # their targets (pgr4-native's scripts/windows/build.ps1 does the same).
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
for line in subprocess.run(['git','-C',repo,'ls-files','-s'],capture_output=True,text=True).stdout.splitlines():
    mode, _, _, path = line.split(None, 3)
    if mode == '120000':
        p = os.path.join(repo, path)
        shutil.copyfile(os.path.normpath(os.path.join(os.path.dirname(p), open(p).read().strip())), p)
"@
    $patches = @(Get-ChildItem "$repo\sdk\patches\pgr4native\*.patch" | Sort-Object Name) +
               @(Get-ChildItem "$repo\sdk\patches\rexnative\*.patch" | Sort-Object Name)
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
    $ffxSrc = ''
    if ($FidelityFxSrc) { $ffxSrc = "`"-DFETCHCONTENT_SOURCE_DIR_FIDELITYFX=$FidelityFxSrc`"" }
    Run "set `"CFLAGS=$flags`" && set `"CXXFLAGS=$flags`" && cmake -S `"$SdkSrc`" -B `"$Dest\sdk`" -G Ninja $ffxSrc -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link `"-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /MAP`" -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_FIDELITYFX=ON -DREXGLUE_USE_VULKAN=ON -DCMAKE_RC_COMPILER=rc `"-DCMAKE_INSTALL_PREFIX=$Dest\sdk-install`"" 'sdk-configure.log'
    Run "cmake --build `"$Dest\sdk`" --target install -j $Jobs" 'sdk-build.log'
}

''
'For the game (CMake):'
"  -DCMAKE_PREFIX_PATH=$Dest\sdk-install"
'  rexnative_add(<target>'
"      REXGLUE_SDK_SRC $SdkSrc"
"      FIDELITYFX_SDK  $FidelityFx"
'      DLSS_SDK        <NVIDIA DLSS SDK>   DLSS_NR <DLSS 5 headers>'
'      LAYER_DIR       <the game''s layer: python -m rexnative adapt>)'
