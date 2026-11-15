# A game kept running between experiments: start it once (and drive it to where the problem
# is), then send it ReXNative's run-time switches, take window captures and read its log, each in
# a few seconds, without starting the game again. Only a rebuild needs a new start.
#
#   scripts\live.ps1 start -Dir <game's build folder> -Exe ironbrigade.exe `
#       -Extra "--rexnative=true" -Route "10:hold:ENTER:150,18:hold:ENTER:150,26:hold:SPACE:150"
#   scripts\live.ps1 shot before          # <live>\shots\before.png
#   scripts\live.ps1 debug "1"            # draw-debug.txt: depth off (see below)
#   scripts\live.ps1 debug "16" -Pulse    # log the next frame's draws, then clear the switch
#   scripts\live.ps1 snap "6160EBE04B693B5E 804E2F1E4E062B9A"   # targets after those shaders
#   scripts\live.ps1 trace game           # push-buffer trace of the next frame (the tap)
#   scripts\live.ps1 keys "0:hold:SPACE:150,3:hold:ENTER:150"
#   scripts\live.ps1 log "failed|removed" 20
#   scripts\live.ps1 status | stop
# With -RenderDoc at start the game runs under RenderDoc (renderdoccmd from -RenderDocDir or
# $env:REXNATIVE_RENDERDOC): `capture` then takes the next frame (F12) into <Dir>\live\rdoc, for
# scripts\rdoc.ps1 (draw list, pixel history, textures) with the game closed or still running.
#
# The game runs in a 1280x720 window (full screen slowed the whole PC down) with
# --rexnative_capture_dir, --rexnative_d3d_control and --rexnative_d3d_trace_dir in <Dir>\live,
# so every switch works without a restart:
#   draw-debug.txt "<mask> [max_draws] [skip_from] [skip_to] [shader hash]...", read every 15
#   frames (renderer/src/native/draw.cpp): 1 depth/stencil off, 2 cull off, 4 blend off, 8 alpha
#   test off, 16 log the next frame's draws, 32 upload every draw, 64 depth bias off, 128 only the
#   listed shaders, 256 snapshot the targets after the first draw of each listed shader (or after
#   draw skip_from), 512 stencil off (depth kept), 1024 depth test always passes (writes and
#   stencil kept), 2048 the same for draws that test depth without writing it. Draws in
#   [skip_from, skip_to) are skipped. 16 and 256 act when they are set,
#   so -Pulse clears the file again.
# Keys: hold:<keys joined by +>:<ms> with real key-down/up (SPACE, ENTER, TAB, arrows, letters;
# with --mnk_mode Space is A and Enter Start), or SendKeys text. They go to the game only while
# its window is in front.
param(
    [Parameter(Position = 0)][string]$Command = 'status',
    [Parameter(Position = 1)][string]$Arg = '',
    [Parameter(Position = 2)][string]$Arg2 = '',
    [string]$Dir = '',
    [string]$Exe = '',
    [string]$Extra = '',
    [string]$Route = '',
    [switch]$Pulse,
    [switch]$Force,
    [switch]$RenderDoc,
    [string]$RenderDocDir = $env:REXNATIVE_RENDERDOC
)
$ErrorActionPreference = 'Stop'
$stateDir = Join-Path $env:LOCALAPPDATA 'rexnative-live'
$statePath = Join-Path $stateDir 'state.json'

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
if (-not ('RexLiveWin' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RexLiveWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    public static void Front(IntPtr h) {
        IntPtr fg = GetForegroundWindow();
        if (fg == h) return;
        uint me = GetCurrentThreadId(), other = GetWindowThreadProcessId(fg, IntPtr.Zero);
        bool att = other != 0 && other != me && AttachThreadInput(me, other, true);
        ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
        if (att) AttachThreadInput(me, other, false);
    }
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    public static void Key(byte vk, bool down) {
        keybd_event(vk, (byte)MapVirtualKey(vk, 0), down ? 0u : 2u, UIntPtr.Zero);
    }
}
'@
}

function Read-State {
    if (-not (Test-Path $statePath)) { return $null }
    return Get-Content $statePath -Raw | ConvertFrom-Json
}

function Get-Game($s) {
    if (-not $s) { throw 'no live game: scripts\live.ps1 start -Dir <game dir> -Exe <exe>' }
    $p = Get-Process -Id $s.pid -ErrorAction SilentlyContinue
    if (-not $p -or $p.Path -ne $s.exe) { throw "the live game ($($s.exe), pid $($s.pid)) is not running" }
    return $p
}

function Get-Window($p) {
    for ($i = 0; $i -lt 50; ++$i) {
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) { return $p.MainWindowHandle }
        Start-Sleep -Milliseconds 200
    }
    throw 'the game has no window'
}

function Send-Schedule($p, [string]$schedule) {
    # "<seconds from now>:<key>" items, in order.
    $start = Get-Date
    foreach ($k in ($schedule -split ',' | Where-Object { $_ })) {
        $i = $k.IndexOf(':'); $at = [double]$k.Substring(0, $i); $key = $k.Substring($i + 1)
        while (((Get-Date) - $start).TotalSeconds -lt $at) {
            Start-Sleep -Milliseconds 100
            if ($p.HasExited) { throw ('the game exited: 0x{0:X8}' -f $p.ExitCode) }
        }
        $h = Get-Window $p
        [RexLiveWin]::Front($h); Start-Sleep -Milliseconds 150
        if ([RexLiveWin]::GetForegroundWindow() -ne $h) { "skipped '$key' at ${at}s: the game is not in front"; continue }
        if ($key -like 'hold:*') {
            $parts = $key.Split(':'); $names = $parts[1].Split('+'); $ms = [int]$parts[2]
            $map = @{ SPACE = 0x20; ENTER = 0x0D; UP = 0x26; DOWN = 0x28; LEFT = 0x25; RIGHT = 0x27; TAB = 0x09; ESC = 0x1B }
            $vks = $names | ForEach-Object { if ($map.ContainsKey($_)) { [byte]$map[$_] } else { [byte][char]$_.ToUpper() } }
            foreach ($vk in $vks) { [RexLiveWin]::Key($vk, $true) }
            Start-Sleep -Milliseconds $ms
            foreach ($vk in $vks) { [RexLiveWin]::Key($vk, $false) }
            "held $($parts[1]) $ms ms at ${at}s"
        } else {
            [System.Windows.Forms.SendKeys]::SendWait($key); "key '$key' at ${at}s"
        }
    }
}

function Save-Shot($p, [string]$path) {
    $h = Get-Window $p
    $r = New-Object RexLiveWin+RECT; [RexLiveWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [RexLiveWin]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
    $black = $true
    foreach ($pt in @(@(0.3, 0.3), @(0.5, 0.5), @(0.7, 0.6), @(0.5, 0.8), @(0.2, 0.7))) {
        $c = $bmp.GetPixel([int]($bmp.Width * $pt[0]), [int]($bmp.Height * $pt[1]))
        if ($c.R + $c.G + $c.B -gt 0) { $black = $false; break }
    }
    if ($black) {  # a swapchain PrintWindow cannot read: copy the screen with the window in front
        [RexLiveWin]::Front($h); Start-Sleep -Milliseconds 300
        $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
    $path
}

function Set-Debug($s, [string]$line) {
    Set-Content -Path (Join-Path $s.live 'cap\draw-debug.txt') -Value $line -Encoding ascii
    Start-Sleep -Milliseconds 1200  # read every 15 frames
    if ($Pulse) {
        Set-Content -Path (Join-Path $s.live 'cap\draw-debug.txt') -Value '0' -Encoding ascii
        Start-Sleep -Milliseconds 1200
    }
}

$s = Read-State
switch ($Command) {
    'start' {
        if ($s -and -not $Force) {
            $old = Get-Process -Id $s.pid -ErrorAction SilentlyContinue
            if ($old -and $old.Path -eq $s.exe) { throw "a live game is running (pid $($s.pid)): stop it or pass -Force" }
        }
        if (-not $Dir -or -not $Exe) { throw 'start needs -Dir and -Exe' }
        $exePath = (Resolve-Path (Join-Path $Dir $Exe)).Path
        $live = Join-Path $Dir 'live'
        foreach ($d in 'cap', 'trace', 'shots', 'rdoc') { New-Item -ItemType Directory -Force (Join-Path $live $d) | Out-Null }
        Set-Content -Path (Join-Path $live 'cap\draw-debug.txt') -Value '0' -Encoding ascii
        $args = @('--log_file=live\live.log', '--mnk_mode=true', '--fullscreen=false',
                  '--window_width=1280', '--window_height=720',
                  "--rexnative_capture_dir=$live\cap", "--rexnative_d3d_control=$live\control.txt",
                  "--rexnative_d3d_trace_dir=$live\trace") + ($Extra -split ' ' | Where-Object { $_ })
        if ($RenderDoc) {
            $rdcmd = Join-Path $RenderDocDir 'renderdoccmd.exe'
            if (-not $RenderDocDir -or -not (Test-Path $rdcmd)) { throw 'RenderDoc not found: -RenderDocDir or REXNATIVE_RENDERDOC' }
            $name = [IO.Path]::GetFileNameWithoutExtension($exePath)
            $before = @(Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
            Start-Process $rdcmd -ArgumentList (@('capture', '-d', $Dir, '-c', "$live\rdoc\cap", $exePath) + $args) -WorkingDirectory $Dir -WindowStyle Hidden | Out-Null
            $p = $null
            for ($i = 0; $i -lt 100 -and -not $p; ++$i) {
                Start-Sleep -Milliseconds 200
                $p = Get-Process -Name $name -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | Select-Object -First 1
            }
            if (-not $p) { throw 'the game did not start under RenderDoc' }
        } else {
            $p = Start-Process $exePath -ArgumentList $args -WorkingDirectory $Dir -PassThru
        }
        New-Item -ItemType Directory -Force $stateDir | Out-Null
        $s = [pscustomobject]@{ pid = $p.Id; exe = $exePath; dir = $Dir; live = $live; started = (Get-Date).ToString('o'); extra = $Extra; renderdoc = [bool]$RenderDoc }
        $s | ConvertTo-Json | Set-Content $statePath -Encoding utf8
        "started pid $($p.Id): $exePath $($args -join ' ')"
        if ($Route) { Send-Schedule $p $Route; 'route done' }
    }
    'keys' { Send-Schedule (Get-Game $s) $Arg }
    'shot' {
        $p = Get-Game $s
        $name = if ($Arg) { $Arg } else { (Get-Date).ToString('HHmmss') }
        Save-Shot $p (Join-Path $s.live "shots\$name.png")
    }
    'debug' { Get-Game $s | Out-Null; Set-Debug $s $Arg; "draw-debug.txt: $Arg$(if ($Pulse) { ' (then 0)' })" }
    'snap' {
        # Snapshots of the bound targets after the first draw of each listed shader (up to 8),
        # or with a number, after that draw index.
        Get-Game $s | Out-Null
        $line = if ($Arg -match '^\d+$') { "256 0 $Arg 0" } else { "256 0 0 0 $Arg" }
        $Pulse = $true; Set-Debug $s $line
        Get-ChildItem (Join-Path $s.live 'cap') -Filter 'snap-*.txt' | Sort-Object LastWriteTime -Descending |
            Select-Object -First 12 | ForEach-Object { $_.FullName }
    }
    'drawlog' {
        Get-Game $s | Out-Null
        $Pulse = $true; Set-Debug $s '16'
        (Get-ChildItem (Join-Path $s.live 'cap') -Filter 'draws-f*.txt' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
    }
    'control' {
        Get-Game $s | Out-Null
        Set-Content -Path (Join-Path $s.live 'control.txt') -Value (($Arg -split ';') -join "`n") -Encoding ascii
        Start-Sleep -Milliseconds 1500
        "control: $Arg"
    }
    'trace' {
        Get-Game $s | Out-Null
        $tag = if ($Arg) { $Arg } else { 'frame' }
        $frames = if ($Arg2) { $Arg2 } else { '1' }
        Set-Content -Path (Join-Path $s.live 'control.txt') -Value "tap on`ntrace $tag $frames" -Encoding ascii
        Start-Sleep -Milliseconds 3000
        Set-Content -Path (Join-Path $s.live 'control.txt') -Value 'tap off' -Encoding ascii
        Start-Sleep -Milliseconds 1500
        Get-ChildItem (Join-Path $s.live 'trace') -Filter "rexnative-d3d-$tag-f*.txt" | Sort-Object LastWriteTime -Descending |
            Select-Object -First ([int]$frames) | ForEach-Object { $_.FullName }
    }
    'capture' {
        # RenderDoc's capture key in the game window; waits for the new capture file.
        $p = Get-Game $s
        if (-not $s.renderdoc) { throw 'the live game was not started with -RenderDoc' }
        $dir = Join-Path $s.live 'rdoc'
        $before = @(Get-ChildItem $dir -Filter *.rdc | ForEach-Object { $_.FullName })
        $h = Get-Window $p
        [RexLiveWin]::Front($h); Start-Sleep -Milliseconds 200
        if ([RexLiveWin]::GetForegroundWindow() -ne $h) { throw 'the game is not in front' }
        [RexLiveWin]::Key(0x7B, $true); Start-Sleep -Milliseconds 120; [RexLiveWin]::Key(0x7B, $false)
        $new = $null
        for ($i = 0; $i -lt 300 -and -not $new; ++$i) {
            Start-Sleep -Milliseconds 200
            $new = Get-ChildItem $dir -Filter *.rdc | Where-Object { $before -notcontains $_.FullName } | Select-Object -First 1
        }
        if (-not $new) { throw 'no capture within 60 s' }
        # Written once its size stops changing.
        do { $len = $new.Length; Start-Sleep -Milliseconds 700; $new.Refresh() } while ($new.Length -ne $len)
        if ($Arg) { $dest = Join-Path $dir "$Arg.rdc"; Move-Item -Force $new.FullName $dest; $dest } else { $new.FullName }
    }
    'log' {
        if (-not $s) { throw 'no live game' }
        $path = Join-Path $s.live 'live.log'
        $n = if ($Arg2) { [int]$Arg2 } else { 30 }
        $lines = Get-Content -LiteralPath $path -Tail 4000
        if ($Arg) { $lines = $lines | Select-String -Pattern $Arg | ForEach-Object { $_.Line } }
        $lines | Select-Object -Last $n
    }
    'status' {
        if (-not $s) { 'no live game'; break }
        $p = Get-Process -Id $s.pid -ErrorAction SilentlyContinue
        if ($p -and $p.Path -eq $s.exe) {
            "running: pid $($s.pid), $($s.exe), since $($s.started), extra '$($s.extra)'"
        } else { "not running (last: $($s.exe))" }
    }
    'stop' {
        if ($s) { Stop-Process -Id $s.pid -Force -ErrorAction SilentlyContinue; Remove-Item $statePath -ErrorAction SilentlyContinue }
        'stopped'
    }
    default { throw "unknown command '$Command' (start, keys, shot, debug, snap, drawlog, control, trace, capture, log, status, stop)" }
}
