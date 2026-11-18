# Keeps ReXNative's ReXGlue SDK current: moves sdk/rexglue.lock and its patch series to a newer
# ReXGlue commit, by default the newest nightly prerelease (rexglue/rexglue-sdk publishes
# nightly-YYYYMMDD-<commit> on each day its development branch moves; releases come from it).
#
#   scripts\update-sdk.ps1 [-Ref nightly|<tag>|<commit>] [-Dest <dir>] [-Check] [-Force]
#                          [-Games <work dir>,...] [-Seconds 60] [-Promote] [-Jobs 10]
#                          [-Llvm <LLVM 20 bin>] [-FidelityFx <dir>] [-VulkanLib <vulkan-1.lib>]
#
#   1. Patch: a clone of its own (<Dest>\rexglue-sdk) at that commit, with the lock's patches
#      applied in order as commits (git am --3way). A patch whose changes the commit already has
#      is dropped (upstream took it). A patch that conflicts stops the run with the clone in the
#      middle of `git am`: fix the files and `git -C <Dest>\rexglue-sdk am --continue`, or
#      `am --skip` when the commit has its change in another form, then run again with the same
#      -Ref. The patches already in the clone are recognised by their subject and not applied
#      twice; the one skipped counts as dropped.
#   2. Build: scripts\build-sdk.ps1 -Tests builds and installs it (<Dest>\sdk, <Dest>\sdk-install)
#      with ReXGlue's unit tests, and ctest runs them. The "base memory module" tests fail in a
#      Windows build of plain ReXGlue too; they are reported apart and do not fail the run.
#   3. -Games: each game work folder is built against it with `python -m rexnative project
#      --build` (codegen again, then the build, in place: the game then runs on this SDK) and
#      started for -Seconds; it must still be running and its log must show drawn frames.
#   4. -Promote (only when 2 and 3 passed, or with -Force, and when the series and the lock have
#      no uncommitted changes, which would be someone's patch in progress): sdk/rexglue.lock and
#      the patch files are written again from the clone, the dropped ones deleted, for a commit.
#
#   -Check   step 1 only: is there a commit newer than the lock, and do the patches apply.
#   -Force   go on when the lock is already at that commit, and promote despite failures.
#
# Every run writes <Dest>\update-status.json (what was applied, dropped, built, tested, started)
# and <Dest>\update-status.txt (the same for a person). The game builds need the paths
# build-sdk.ps1 prints and $env:REXNATIVE_DLSS_SDK / $env:REXNATIVE_DLSS_NR, as `rexnative
# project` does; the SDK paths are set here to the new SDK.
param(
    [string]$Ref = 'nightly',
    [string]$Dest = "$PSScriptRoot\..\build\rexglue-next",
    [switch]$Check,
    [switch]$Force,
    [string[]]$Games = @(),
    [int]$Seconds = 60,
    [switch]$Promote,
    [int]$Jobs = 10,
    [string]$Llvm = $env:REXNATIVE_LLVM,
    [string]$FidelityFx = $env:REXNATIVE_FIDELITYFX,
    [string]$VulkanLib = $env:REXNATIVE_VULKAN_LIB
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$Dest = [IO.Path]::GetFullPath($Dest)
$src = "$Dest\rexglue-sdk"
New-Item -ItemType Directory -Force "$Dest\logs" | Out-Null

function Lock([string]$path) {
    $lock = @{}
    Get-Content $path | Where-Object { $_ -match '^(\w+)=(.*)$' } | ForEach-Object { $lock[$Matches[1]] = $Matches[2] }
    return $lock
}
# git in the clone. Its stderr (progress, conflict notes) stays out of PowerShell 5.1's error
# stream, where ErrorActionPreference Stop would make it a failure; git.exe by name, as a function
# named git would be called in its place.
function InClone {
    $ErrorActionPreference = 'Continue'
    $out = & git.exe -C $src @args 2>&1 | ForEach-Object { "$_" }
    $script:gitExit = $LASTEXITCODE
    return $out
}
function Subject([string]$patch) {
    # The Subject header, with its folded lines and without "[PATCH ...]", as git am commits it.
    $lines = Get-Content $patch -TotalCount 40
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match '^Subject: (.*)$') {
            $s = $Matches[1]
            while ($i + 1 -lt $lines.Count -and $lines[$i + 1] -match '^\s+(.*)$') { $s += ' ' + $Matches[1]; ++$i }
            return ($s -replace '^\[PATCH[^\]]*\]\s*', '').Trim()
        }
    }
    throw "no Subject in $patch"
}

$lockPath = "$repo\sdk\rexglue.lock"
$lock = Lock $lockPath
$series = if ($lock.REXGLUE_PATCHES) { $lock.REXGLUE_PATCHES } else { 'v0.11' }
$patchDir = "$repo\sdk\patches\$series"
$status = [ordered]@{
    when = (Get-Date).ToString('s'); ref = $Ref; tag = ''; commit = ''
    lock_commit = $lock.REXGLUE_COMMIT; lock_tag = "$($lock.REXGLUE_TAG)"; series = $series
    up_to_date = $false; applied = @(); dropped = @(); conflict = ''
    build = 'not run'; tests = [ordered]@{ result = 'not run'; failed = @(); known = @() }
    games = @(); promoted = $false; promote_blocked = ''; result = ''
}
function Save([string]$result) {
    $status.result = $result
    $status | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "$Dest\update-status.json"
    $t = @("ReXGlue SDK update, $($status.when)",
        "  target: $($status.tag) $($status.commit) (lock: $($status.lock_tag) $($status.lock_commit))",
        "  patches: $($status.applied.Count) applied, $($status.dropped.Count) dropped (already upstream)$(if ($status.conflict) { ", conflict in $($status.conflict)" })")
    foreach ($d in $status.dropped) { $t += "    dropped: $d" }
    $t += "  build: $($status.build); tests: $($status.tests.result)"
    foreach ($f in $status.tests.failed) { $t += "    failed: $f" }
    foreach ($g in $status.games) { $t += "  game $($g.name): $($g.result)" }
    $t += "  promoted: $($status.promoted)"
    $t += "  result: $result"
    $t | Set-Content -Encoding UTF8 "$Dest\update-status.txt"
    $t
}

# -- 1. The target commit, the clone, the patches -------------------------------------------------
if (-not (Test-Path "$src\.git")) {
    git clone -q --no-checkout --filter=blob:none $lock.REXGLUE_URL $src
    if ($LASTEXITCODE) { throw 'git clone of ReXGlue failed' }
    git -C $src config core.autocrlf false
}
# The clone's own committer for git am (and its --abort, which needs one too): its commits only
# carry the patches, whose authors stay theirs.
InClone config user.name 'ReXNative update' | Out-Null
InClone config user.email 'update@rexnative.invalid' | Out-Null
InClone fetch -q --tags --force origin | Out-Null
if ($gitExit) { throw 'git fetch of ReXGlue failed' }
$tag = ''
if ($Ref -eq 'nightly') {
    try {
        $rel = Invoke-RestMethod -UseBasicParsing 'https://api.github.com/repos/rexglue/rexglue-sdk/releases?per_page=40'
        $tag = ($rel | Where-Object { $_.prerelease -and $_.tag_name -like 'nightly-*' } |
            Sort-Object { [datetime]$_.published_at } -Descending | Select-Object -First 1).tag_name
    } catch { }
    if (-not $tag) {
        # Without the API: the newest nightly tag by name (its date comes first).
        $tag = (InClone tag --list 'nightly-*' | Sort-Object | Select-Object -Last 1)
    }
    if (-not $tag) { throw 'no nightly tag found' }
    $target = $tag
} else {
    $target = $Ref
    if ((InClone tag --list $Ref)) { $tag = $Ref }
}
$commit = (InClone rev-parse --verify -q "$target^{commit}" | Select-Object -First 1)
if ($gitExit -or -not $commit) { throw "ReXGlue $target not found" }
$status.tag = $tag
$status.commit = $commit
if ($commit -eq $lock.REXGLUE_COMMIT -and -not $Force) {
    $status.up_to_date = $true
    Save "up to date: the lock is at $target already" | Write-Output
    exit 0
}

$stateFile = "$Dest\update-state.txt"
# update-state.txt: the commit the clone was set to, then for that commit the patches dropped
# ("drop <name>") and the one the last run stopped at ("conflict <name>").
$state = @(if (Test-Path $stateFile) { Get-Content $stateFile }) + @('')
$prev = $state[0].Trim()
$knownDropped = @()
$prevConflict = ''
if ($prev -eq $commit) {
    foreach ($l in $state[1..($state.Count - 1)]) {
        if ($l -match '^drop (.+)$') { $knownDropped += $Matches[1].Trim() }
        elseif ($l -match '^conflict (.+)$') { $prevConflict = $Matches[1].Trim() }
    }
}
function SaveState([string]$conflict) {
    $lines = @($commit) + @($status.dropped | ForEach-Object { "drop $_" })
    if ($conflict) { $lines += "conflict $conflict" }
    Set-Content -Encoding ASCII $stateFile $lines
}
$amDir = (InClone rev-parse --git-path rebase-apply | Select-Object -First 1)
if (-not [IO.Path]::IsPathRooted($amDir)) { $amDir = Join-Path $src $amDir }
if ($prev -eq $commit -and (Test-Path $amDir)) {
    throw "the clone is in the middle of git am: finish it (git -C $src am --continue, or --skip / --abort) and run again"
}
if ($prev -ne $commit) {
    if (Test-Path $amDir) { InClone am --abort | Out-Null }
    InClone checkout -q -f --detach $commit | Out-Null
    if ($gitExit) { throw "checkout of $commit failed" }
    InClone clean -q -fd | Out-Null
    InClone submodule update -q --init --recursive --depth 1 --jobs 8 | Out-Null
    if ($gitExit) {
        # A submodule pinned below its branch tip is not in a depth 1 fetch.
        InClone submodule update -q --init --recursive --jobs 8 | Out-Null
        if ($gitExit) { throw 'submodule update failed' }
    }
    InClone submodule foreach -q --recursive 'git config core.autocrlf false' | Out-Null
    # libmspack's git symlinks become text placeholders without Windows developer mode: copies of
    # their targets take their place (as build-sdk.ps1 does).
    python -c @"
import subprocess, os, shutil
repo = r'$src\thirdparty\libmspack'
if os.path.isdir(repo):
    for line in subprocess.run(['git','-C',repo,'ls-files','-s'],capture_output=True,text=True).stdout.splitlines():
        mode, _, _, path = line.split(None, 3)
        if mode == '120000':
            p = os.path.join(repo, path)
            shutil.copyfile(os.path.normpath(os.path.join(os.path.dirname(p), open(p).read().strip())), p)
"@
    Set-Content -Encoding ASCII $stateFile $commit
}

$subjects = @(InClone log --format=%s "$commit..HEAD")
$patches = @(Get-ChildItem "$patchDir\*.patch" | Sort-Object Name)
foreach ($p in $patches) {
    $subject = Subject $p.FullName
    if ($subjects -contains $subject) { $status.applied += $p.Name; continue }
    if ($knownDropped -contains $p.Name) { $status.dropped += $p.Name; continue }
    if ($p.Name -eq $prevConflict) {
        # The patch the last run stopped at is not in the clone: it was skipped (git am --skip),
        # its change being upstream in another form.
        $status.dropped += $p.Name
        continue
    }
    InClone am --3way -q $p.FullName | Out-Null
    if (-not $gitExit) { $status.applied += $p.Name; continue }
    InClone am --abort | Out-Null
    InClone apply --reverse --check $p.FullName | Out-Null
    if (-not $gitExit) { $status.dropped += $p.Name; continue }
    # Left in the middle of git am for a person (or a session) to resolve.
    InClone am --3way $p.FullName | Out-File -Encoding UTF8 "$Dest\logs\conflict.log"
    $status.conflict = $p.Name
    SaveState $p.Name
    Save "stopped: $($p.Name) conflicts with $target (see $Dest\logs\conflict.log; fix it in $src, git am --continue, run again)" | Write-Output
    exit 2
}
SaveState ''
if ($Check) {
    Save "checked: $target takes the series ($($status.applied.Count) applied, $($status.dropped.Count) dropped); not built" | Write-Output
    exit 0
}

# -- 2. Build, install, unit tests -----------------------------------------------------------------
$build = @{ Dest = $Dest; SdkSrc = $src; Jobs = $Jobs; Tests = $true }
if ($Llvm) { $build.Llvm = $Llvm }
if ($FidelityFx) { $build.FidelityFx = $FidelityFx }
if ($VulkanLib) { $build.VulkanLib = $VulkanLib }
foreach ($k in @(@('VulkanInclude', 'thirdparty\Vulkan-Headers\include'), @('VmaInclude', 'thirdparty\VulkanMemoryAllocator\include'))) {
    if (Test-Path "$src\$($k[1])") { $build[$k[0]] = "$src\$($k[1])" }
}
try {
    & "$PSScriptRoot\build-sdk.ps1" @build | Out-File -Encoding UTF8 "$Dest\logs\build-sdk.txt"
    $status.build = 'ok'
} catch {
    $status.build = "failed: $($_.Exception.Message)"
    Save "stopped: the SDK does not build" | Write-Output
    exit 3
}
$ctest = "$Dest\logs\ctest.log"
if (Test-Path $ctest) {
    $failed = @(Select-String -Path $ctest -Pattern '^\s+\d+ - (.+?) \((Failed|SEGFAULT|Exception|Timeout|Not Run|Subprocess aborted)\)(.*)$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value + $_.Matches[0].Groups[3].Value })
    $failed = @($failed | Select-Object -Unique)
    $status.tests.known = @($failed | Where-Object { $_ -match 'base memory module' })
    $status.tests.failed = @($failed | Where-Object { $_ -notmatch 'base memory module' })
    $summary = (Select-String -Path $ctest -Pattern 'tests passed, (\d+) tests failed out of (\d+)' | Select-Object -First 1)
    $status.tests.result = if ($status.tests.failed.Count) { "$($status.tests.failed.Count) unexpected failures" } elseif ($summary) { $summary.Line.Trim() } else { 'no summary' }
}

# -- 3. Games built against it and started --------------------------------------------------------
$env:REXNATIVE_REXGLUE = "$Dest\sdk-install\bin\rexglue.exe"
$env:REXNATIVE_SDK_INSTALL = "$Dest\sdk-install"
$env:REXNATIVE_SDK_SRC = $src
if ($FidelityFx) { $env:REXNATIVE_FIDELITYFX = $FidelityFx }
if ($Llvm) { $env:REXNATIVE_LLVM = $Llvm }
foreach ($w in $Games) {
    $w = [IO.Path]::GetFullPath($w)
    $name = (Get-ChildItem "$w\*_manifest.toml" | Select-Object -First 1).BaseName -replace '_manifest$', ''
    $g = [ordered]@{ name = (Split-Path $w -Leaf); built = $false; running = $false; frames = 0; result = '' }
    $log = "$Dest\logs\game-$($g.name).log"
    Push-Location $repo
    try {
        cmd /c "python -m rexnative project `"$w`" --build > `"$log`" 2>&1"
        $g.built = ($LASTEXITCODE -eq 0)
    } finally { Pop-Location }
    $exe = "$w\run\$name.exe"
    if (-not $g.built -or -not (Test-Path $exe)) {
        $g.result = "does not build (see $log)"
        $status.games += $g
        continue
    }
    $started = Get-Date
    $proc = Start-Process -FilePath $exe -WorkingDirectory "$w\run" -PassThru
    Start-Sleep -Seconds $Seconds
    $g.running = -not $proc.HasExited
    if ($g.running) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    $glog = Get-ChildItem "$w\run\logs\*.log" -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $started } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($glog) {
        $f = Select-String -Path $glog.FullName -Pattern 'frames (\d+) \|' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
        if ($f) { $g.frames = ($f | Measure-Object -Maximum).Maximum }
    }
    $g.result = if ($g.running -and $g.frames -ge 150) { "ok: still running after $Seconds s, $($g.frames) frames drawn" }
    elseif (-not $g.running) { "stopped before $Seconds s (exit $($proc.ExitCode)); log $($glog.FullName)" }
    else { "running but only $($g.frames) frames drawn; log $($glog.FullName)" }
    $status.games += $g
}
$gamesOk = -not ($status.games | Where-Object { $_.result -notlike 'ok:*' })
$passed = $status.build -eq 'ok' -and -not $status.tests.failed.Count -and $gamesOk

# -- 4. The lock and the series written again --------------------------------------------------------
# A patch being written elsewhere (not committed yet) would be overwritten: no promotion then.
$pending = @(git -C $repo status --porcelain -- "sdk/patches/$series" sdk/rexglue.lock)
if ($Promote -and $pending.Count) {
    $status.promote_blocked = "uncommitted changes in the series: $($pending -join '; ')"
}
if ($Promote -and ($passed -or $Force) -and -not $pending.Count) {
    $names = @{}
    foreach ($p in $patches) { $names[(Subject $p.FullName)] = $p.Name }
    $tmp = "$Dest\format-patch"
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    $shas = @(InClone rev-list --reverse "$commit..HEAD")
    $n = $patches.Count
    foreach ($sha in $shas) {
        $s = (InClone log -1 --format=%s $sha | Select-Object -First 1)
        $file = $names[$s]
        if (-not $file) {
            # A patch added in the clone (a conflict's fix split off, say): numbered after the rest.
            ++$n
            $slug = ($s -replace '[^A-Za-z0-9]+', '-').Trim('-')
            $file = '{0:D4}-{1}.patch' -f $n, $slug.Substring(0, [Math]::Min(52, $slug.Length))
        }
        InClone format-patch -q -1 $sha -o $tmp | Out-Null
        Move-Item -Force (Get-ChildItem "$tmp\*.patch" | Select-Object -First 1).FullName "$patchDir\$file"
    }
    foreach ($d in $status.dropped) { Remove-Item -Force "$patchDir\$d" }
    $what = if ($tag) { $tag } else { $commit.Substring(0, 8) }
    @("# The ReXGlue SDK ReXNative's renderer is built and tested against: ReXGlue $what, with",
        "# sdk/patches/$series/ applied in order (scripts/build-sdk.ps1; scripts/update-sdk.ps1 moves it).",
        "REXGLUE_URL=$($lock.REXGLUE_URL)",
        "REXGLUE_COMMIT=$commit",
        "REXGLUE_TAG=$tag",
        "REXGLUE_PATCHES=$series") | Set-Content -Encoding ASCII $lockPath
    $status.promoted = $true
}
$result = if ($status.promoted) { "promoted to ${target}: commit sdk/rexglue.lock and sdk/patches/$series" }
elseif ($passed -and $status.promote_blocked) { "passed, not promoted: $($status.promote_blocked)" }
elseif ($passed) { "passed: $target builds, its tests and games pass (run with -Promote to move the lock)" }
else { "failed: see the build, tests and games above" }
Save $result | Write-Output
if (-not $passed) { exit 4 }
