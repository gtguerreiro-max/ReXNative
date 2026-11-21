# A game kept running between experiments: start it once (and drive it to where the problem
# is), then send it ReXNative's run-time switches, take window captures and read its log, each in
# a few seconds, without starting the game again. Only a rebuild needs a new start.
#
#   scripts\live.ps1 start -Dir <game's build folder> -Exe ironbrigade.exe `
#       -Extra "--rexnative=true" -Route "10:hold:ENTER:150,18:hold:ENTER:150,26:hold:SPACE:150"
#   scripts\live.ps1 shot before          # <live>\shots\before.png (window capture)
#   scripts\live.ps1 frame before         # the same from the renderer itself: no focus needed
#   scripts\live.ps1 debug "1"            # draw-debug.txt: depth off (see below)
#   scripts\live.ps1 debug "16" -Pulse    # log the next frame's draws, then clear the switch
#   scripts\live.ps1 snap "6160EBE04B693B5E 804E2F1E4E062B9A"   # targets after those shaders
#   scripts\live.ps1 trace game           # push-buffer trace of the next frame (the tap)
#   scripts\live.ps1 keys "0:hold:SPACE:150,3:hold:ENTER:150"
#   scripts\live.ps1 click 535 403        # a left click at x,y of the window as `shot` saves it
#   scripts\live.ps1 log "failed|removed" 20
#   scripts\live.ps1 control "renderer stock"   # control-file commands (rexnative_d3d_hooks.cpp),
#                                               # ; between several: here the Xenos plugin draws
#   scripts\live.ps1 status | stop      # stop -Dir <folder>: only if the live game is that one
# With -RenderDoc at start the renderer loads RenderDoc itself (--rexnative_renderdoc=auto: no
# launcher, see renderer/src/native/debug_tools.h), and `capture [N]` takes the next N whole
# guest frames, a capture each, into <Dir>\live\rdoc\<game>-f<frame>.rdc, for scripts\rdoc.ps1
# (`summary` first: the frame's passes in a few lines). The window need not be in front.
#   scripts\live.ps1 record 4           # a frame trace of the next 4 frames (<live>\trace\*.rxt)
#   scripts\live.ps1 play <trace.rxt>   # the game's executable plays the trace instead of the
#                                       # game, as the live game: every command here works on it
#                                       # (-Extra "--rexnative_trace_play_loops=2 ..." to exit)
#   scripts\live.ps1 compare [threshold] # on a played trace: the resolves of its first frame as
#                                       # the game drew them when it was recorded against the
#                                       # replay's, only the ones apart (default 0.004): a fault
#                                       # the replay does not have is in the first pass listed
#   scripts\live.ps1 audit              # the textures used in the last second: guest memory
#                                       # changed unseen, or the GPU's copy differs from it
#   scripts\live.ps1 idle [frames]      # drops the resolved images not resolved into for that
#                                       # many frames (default 60): a fault that goes with them
#                                       # was an image that outlived its memory
# Two sessions at once: each sets $env:REXNATIVE_LIVE_STATE to a folder of its own, or one's
# start, play and stop act on the other's game (the live game is one per state folder).
# With -Oracle at start the Xenos plugin draws the game and ReXNative draws beside it, and
#   scripts\live.ps1 oracle [threshold] # compares every resolve of the next frame between the
#                                       # two, printing only the ones that differ (the first one
#                                       # is where a pass went wrong; scripts\resolves.py)
#
# The game runs in a 1280x720 window (full screen slowed the whole PC down) with
# --rexnative_capture_dir, --rexnative_d3d_control and --rexnative_d3d_trace_dir in <Dir>\live,
# so every switch works without a restart:
#   draw-debug.txt "<mask> [max_draws] [skip_from] [skip_to] [shader hash]...", read every 15
#   frames (renderer/src/native/draw.cpp): 1 depth/stencil off, 2 cull off, 4 blend off, 8 alpha
#   test off, 16 log the next frame's draws, 32 upload every draw, 64 depth bias off, 128 only the
#   listed shaders, 256 snapshot the targets after the first draw of each listed shader (or after
#   draw skip_from), 512 stencil off (depth kept), 1024 depth test always passes (writes and
#   stencil kept), 2048 the same for draws that test depth without writing it, 4096 skip the
#   draws replayed from recorded command buffers, 8192 skip all the others, 16384 the
#   upscaler's scene without its jitter, 32768 without its motion vector pipelines (with a
#   range: only the draws in it, none skipped, to bisect), 65536 motion vectors without the
#   first vertex (MotionHistory's anchor: no tie-break, no distance limit), 262144 log each
#   motion draw's match ("ReXNative motion: f<frame> #<k>": the k-th draw writing the motion
#   target in a capture of that frame), 524288 motion vectors without the vertex buffer rules
#   (MotionHistory: the draw with this one's buffer first, another's refused), 4194304 without
#   the first vertex of 16-bit components, 2097152 without the camera from a draw later in the
#   frame (MotionHistory::pendable), 1048576 the motion bits in even frames only (an A/B in one
#   capture). Draws in
#   [skip_from, skip_to) are skipped. 16 and 256 act when they are set,
#   so -Pulse clears the file again.
# Keys: hold:<keys joined by +>:<ms> with real key-down/up (SPACE, ENTER, TAB, arrows, letters;
# with --mnk_mode Space is A and Enter Start), or SendKeys text. They go to the game only while
# its window is in front: a Windows prompt that holds the focus (the firewall's "allow access?"
# for a game that opens a socket) is the user's to answer, and the keys are skipped meanwhile.
# pad:<buttons joined by +>:<ms> (A B X Y START BACK UP DOWN LEFT RIGHT LB RB) presses them on the
# game's pad instead, through the control file: no focus needed (a layer with XInputGetState).
# More fields hold the sticks and triggers meanwhile: pad:none:600:ry=32767 looks up for 600 ms
# (lx ly rx ry from -32768 to 32767, lt rt from 0 to 255).
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
    [switch]$Oracle,
    [switch]$Plain
)
$ErrorActionPreference = 'Stop'
# The live game is one per state folder, by default one for the PC. Two sessions working at once
# each set REXNATIVE_LIVE_STATE to a folder of their own, so that neither sees, replaces or stops
# the other's game.
$stateDir = if ($env:REXNATIVE_LIVE_STATE) { $env:REXNATIVE_LIVE_STATE } else { Join-Path $env:LOCALAPPDATA 'rexnative-live' }
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
        if (GetForegroundWindow() != h) {
            // The foreground lock (a notification toast in front, nobody at the PC): a key
            // event lets this process set the foreground. Alt alone, down and up.
            keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero);
            SetForegroundWindow(h);
        }
    }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    public static void Click(int x, int y) {
        SetCursorPos(x, y); System.Threading.Thread.Sleep(60);
        mouse_event(2, 0, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(60);  // left down
        mouse_event(4, 0, 0, 0, UIntPtr.Zero);  // left up
    }
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    public static void Key(byte vk, bool down) {
        // Arrows, Home/End/PageUp/PageDown, Insert/Delete: extended keys. Without the flag they
        // arrive as the numeric keypad's (a game's right stick on the arrows never moves).
        bool ext = (vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E;
        keybd_event(vk, (byte)MapVirtualKey(vk, 0), (down ? 0u : 2u) | (ext ? 1u : 0u), UIntPtr.Zero);
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
        if ($key -like 'pad:*') {
            # pad:<buttons joined by +>:<ms>: the control file's `pad`, on the game's pad (its
            # XInputGetState): no focus needed.
            $parts = $key.Split(':')
            $ms = if ($parts.Count -gt 2) { $parts[2] } else { '150' }
            $axes = if ($parts.Count -gt 3) { ' ' + ($parts[3..($parts.Count - 1)] -join ' ') } else { '' }
            Add-Content -Path (Join-Path $s.live 'control.txt') -Value "pad $($parts[1])$axes $ms" -Encoding ascii
            "pad $($parts[1])$axes $ms ms at ${at}s"
            continue
        }
        $h = Get-Window $p
        [RexLiveWin]::Front($h); Start-Sleep -Milliseconds 150
        if ([RexLiveWin]::GetForegroundWindow() -ne $h) { "skipped '$key' at ${at}s: the game is not in front"; continue }
        if ($key -like 'hold:*') {
            $parts = $key.Split(':'); $names = $parts[1].Split('+'); $ms = [int]$parts[2]
            $map = @{ SPACE = 0x20; ENTER = 0x0D; UP = 0x26; DOWN = 0x28; LEFT = 0x25; RIGHT = 0x27; TAB = 0x09; ESC = 0x1B; BACKSPACE = 0x08; SHIFT = 0x10; CTRL = 0x11; ALT = 0x12 }
            for ($f = 1; $f -le 12; $f++) { $map["F$f"] = 0x6F + $f }  # F1 (the settings menu) = 0x70
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
$more = @()
if ($Command -eq 'play') {
    # The game's own executable plays a frame trace instead of the game. The game is the trace's:
    # <game's folder>\live\trace\<executable's name>-f<frame>.rxt, as `record` writes it (else
    # -Dir and -Exe). Never the live game of another folder: the live state is one for the PC,
    # and that game may be another session's.
    if (-not $Arg) { throw 'play needs a trace: scripts\live.ps1 play <trace.rxt>' }
    $trace = (Resolve-Path $Arg).Path
    if (-not $Dir) { $Dir = Split-Path (Split-Path (Split-Path $trace)) }
    if (-not $Exe) { $Exe = ([IO.Path]::GetFileNameWithoutExtension($trace) -replace '-f\d+$', '') + '.exe' }
    if (-not (Test-Path (Join-Path $Dir $Exe))) { throw "play: no $Exe in $Dir (pass -Dir and -Exe: the game the trace was recorded from)" }
    $more = @("--rexnative_trace_play=$trace")
    $same = $s -and ([IO.Path]::GetFullPath($s.dir).TrimEnd('\') -eq [IO.Path]::GetFullPath($Dir).TrimEnd('\'))
    $RenderDoc = $RenderDoc -or ($same -and $s.renderdoc)
    # The live game of the same folder is replaced; another one is left alone (start refuses).
    if ($same) { $Force = $true }
    $Command = 'start'
}
function Get-LogPath($s) {
    $path = Join-Path $s.live 'live.log'
    # ReXGlue v0.11 writes --log_file=live\live.log as logs\live\live.log_NNN.log.
    $rotated = Get-ChildItem (Join-Path $s.dir 'logs\live') -Filter 'live.log*' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($rotated -and (-not (Test-Path $path) -or $rotated.LastWriteTime -gt (Get-Item $path).LastWriteTime)) {
        $path = $rotated.FullName
    }
    return $path
}
function Wait-Stable([string]$path) {
    # Written once its size is above zero and stops changing.
    $len = -1
    for ($i = 0; $i -lt 100; ++$i) {
        Start-Sleep -Milliseconds 400
        $f = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
        if ($f -and $f.Length -gt 0 -and $f.Length -eq $len) { return }
        if ($f) { $len = $f.Length }
    }
}
switch ($Command) {
    'start' {
        if ($s) {
            $old = Get-Process -Id $s.pid -ErrorAction SilentlyContinue
            if ($old -and $old.Path -eq $s.exe) {
                if (-not $Force) { throw "a live game is running (pid $($s.pid)): stop it or pass -Force" }
                # -Force replaces it: the old game is closed, not left running beside the new one.
                Stop-Process -Id $s.pid -Force -ErrorAction SilentlyContinue
                $old.WaitForExit(10000) | Out-Null
            }
        }
        if (-not $Dir -or -not $Exe) { throw 'start needs -Dir and -Exe' }
        $exePath = (Resolve-Path (Join-Path $Dir $Exe)).Path
        $live = Join-Path $Dir 'live'
        foreach ($d in 'cap', 'trace', 'shots', 'rdoc', 'resolves', 'resolves-play') { New-Item -ItemType Directory -Force (Join-Path $live $d) | Out-Null }
        Set-Content -Path (Join-Path $live 'cap\draw-debug.txt') -Value '0' -Encoding ascii
        # -Plain: a game without ReXNative (a stock ReXGlue build, which refuses settings it
        # does not know): the window and the keyboard only.
        # The resolve dumps of the game go to <live>\resolves and a played trace's to
        # <live>\resolves-play (`record` and `play` each dump the trace's first frame, for
        # `compare`); the textures keep a hash of what they were uploaded from, for `audit`.
        $dumps = if ($more) { 'resolves-play' } else { 'resolves' }
        Remove-Item (Join-Path $live "$dumps\*") -ErrorAction SilentlyContinue
        $native = if ($Plain) { @() } else {
            @("--rexnative_capture_dir=$live\cap", "--rexnative_d3d_control=$live\control.txt",
              "--rexnative_d3d_trace_dir=$live\trace", "--rexnative_resolve_dump=$live\$dumps",
              '--rexnative_texture_audit=1') }
        # -RenderDoc: the renderer loads it and takes the captures (debug_tools.h).
        if ($RenderDoc) { $native += @('--rexnative_renderdoc=auto', "--rexnative_renderdoc_dir=$live\rdoc") }
        # -Oracle: the Xenos plugin presents and ReXNative draws beside it, on the guest thread;
        # `oracle` dumps a frame's resolves from both.
        if ($Oracle) {
            $native += @('--rexnative=false', '--rexnative_beside_xenos=true', '--rexnative_render_thread=false',
                         '--rexnative_hdr=false')
        }
        $args = @('--log_file=live\live.log', '--mnk_mode=true', '--fullscreen=false',
                  '--window_width=1280', '--window_height=720') + $native + $more +
                ($Extra -split ' ' | Where-Object { $_ })
        $p = Start-Process $exePath -ArgumentList $args -WorkingDirectory $Dir -PassThru
        New-Item -ItemType Directory -Force $stateDir | Out-Null
        $s = [pscustomobject]@{ pid = $p.Id; exe = $exePath; dir = $Dir; live = $live; started = (Get-Date).ToString('o'); extra = $Extra; renderdoc = [bool]$RenderDoc; oracle = [bool]$Oracle; playing = [bool]$more }
        $s | ConvertTo-Json | Set-Content $statePath -Encoding utf8
        "started pid $($p.Id): $exePath $($args -join ' ')"
        if ($Route) { Send-Schedule $p $Route; 'route done' }
    }
    'keys' { Send-Schedule (Get-Game $s) $Arg }
    'click' {
        # A left click at x,y of the window as `shot` saves it (the runtime's ImGui dialogs, such
        # as "Gamer profile not online", take the mouse and not the keys).
        $p = Get-Game $s
        $h = Get-Window $p
        $r = New-Object RexLiveWin+RECT; [RexLiveWin]::GetWindowRect($h, [ref]$r) | Out-Null
        [RexLiveWin]::Front($h); Start-Sleep -Milliseconds 150
        if ([RexLiveWin]::GetForegroundWindow() -ne $h) { throw 'the game is not in front' }
        [RexLiveWin]::Click($r.Left + [int]$Arg, $r.Top + [int]$Arg2)
        "clicked $Arg,$Arg2"
    }
    'shot' {
        $p = Get-Game $s
        $name = if ($Arg) { $Arg } else { (Get-Date).ToString('HHmmss') }
        Save-Shot $p (Join-Path $s.live "shots\$name.png")
    }
    'frame' {
        # The renderer's own capture of the next frame (capture.request in <live>\cap): its
        # f<frame>-native.png copied to <live>\shots\<name>.png. Needs no window in front.
        Get-Game $s | Out-Null
        $cap = Join-Path $s.live 'cap'
        $name = if ($Arg) { $Arg } else { (Get-Date).ToString('HHmmss') }
        $before = Get-Date
        New-Item -ItemType File -Force (Join-Path $cap 'capture.request') | Out-Null
        $f = $null
        for ($i = 0; $i -lt 50 -and -not $f; ++$i) {
            Start-Sleep -Milliseconds 200
            $f = Get-ChildItem $cap -Filter 'f*-native.png' | Where-Object { $_.LastWriteTime -ge $before } |
                Sort-Object LastWriteTime | Select-Object -Last 1
        }
        if (-not $f) { throw 'no capture within 10 s' }
        # Written once its size is above zero and stops changing (a busy disk writes it slowly).
        $len = -1
        for ($i = 0; $i -lt 50; ++$i) {
            Start-Sleep -Milliseconds 300
            $f.Refresh()
            if ($f.Length -gt 0 -and $f.Length -eq $len) { break }
            $len = $f.Length
        }
        $dst = Join-Path $s.live "shots\$name.png"
        Copy-Item $f.FullName $dst -Force
        $dst
    }
    'burst' {
        # A few window captures in a row (-Arg2 of them, default 6, ~100 ms apart): glitches that
        # one still capture does not catch (frame generation, jitter, motion).
        $p = Get-Game $s
        $name = if ($Arg) { $Arg } else { (Get-Date).ToString('HHmmss') }
        $n = if ($Arg2) { [int]$Arg2 } else { 6 }
        for ($i = 0; $i -lt $n; ++$i) {
            Save-Shot $p (Join-Path $s.live "shots\$name-$i.png")
            Start-Sleep -Milliseconds 100
        }
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
        # The next N whole guest frames (default 1), a RenderDoc capture each, taken by the
        # renderer: <live>\rdoc\<game>-f<frame>.rdc, listed in captures.txt as they are written.
        Get-Game $s | Out-Null
        if (-not $s.renderdoc) { throw 'the live game was not started with -RenderDoc' }
        $n = if ($Arg -match '^\d+$') { [int]$Arg } else { 1 }
        $list = Join-Path $s.live 'rdoc\captures.txt'
        $before = if (Test-Path $list) { @(Get-Content $list).Count } else { 0 }
        Add-Content -Path (Join-Path $s.live 'control.txt') -Value "set rexnative_renderdoc_frames $n" -Encoding ascii
        $lines = @()
        for ($i = 0; $i -lt 300 -and $lines.Count -lt $before + $n; ++$i) {
            Start-Sleep -Milliseconds 300
            if (Test-Path $list) { $lines = @(Get-Content $list) }
        }
        if ($lines.Count -le $before) { throw 'no capture within 90 s (see `log RenderDoc`)' }
        $lines | Select-Object -Skip $before | ForEach-Object { ($_ -split ' ', 2)[1] }
    }
    'record' {
        # A frame trace of the next N frames (default 2): what the game hands the renderer and
        # the guest memory it reads, for `play` (renderer/src/native/trace.h).
        Get-Game $s | Out-Null
        $n = if ($Arg -match '^\d+$') { [int]$Arg } else { 2 }
        $start = Get-Date
        Add-Content -Path (Join-Path $s.live 'control.txt') -Value "set rexnative_trace_record $n" -Encoding ascii
        $f = $null
        for ($i = 0; $i -lt 100 -and -not $f; ++$i) {
            Start-Sleep -Milliseconds 300
            $f = Get-ChildItem (Join-Path $s.live 'trace') -Filter '*.rxt' | Where-Object { $_.LastWriteTime -ge $start } |
                Sort-Object LastWriteTime | Select-Object -Last 1
        }
        if (-not $f) { throw 'no trace within 30 s (the render thread must be on; see `log trace`)' }
        Wait-Stable $f.FullName
        $f.Refresh()
        '{0} ({1:n1} MB)' -f $f.FullName, ($f.Length / 1MB)
    }
    'compare' {
        # A played trace against the game it was recorded from: `record` dumped the resolves of
        # the trace's first frame as the game drew it, the player dumps the same frame the third
        # time through; only the resolves apart are printed (scripts\resolves.py --against).
        Get-Game $s | Out-Null
        if (-not $s.playing) { throw 'compare works on a played trace: live.ps1 record 1, then live.ps1 play <trace>' }
        $mine = Join-Path $s.live 'resolves'; $play = Join-Path $s.live 'resolves-play'
        if (-not (Get-ChildItem $mine -Filter 'f*-resolves.jsonl')) { throw "no resolve dump of the game in $mine (record the trace with this live.ps1)" }
        $meta = $null
        for ($i = 0; $i -lt 200 -and -not $meta; ++$i) {
            Start-Sleep -Milliseconds 300
            $meta = Get-ChildItem $play -Filter 'f*-resolves.jsonl' | Select-Object -First 1
        }
        if (-not $meta) { throw 'the player wrote no resolve dump within 60 s' }
        Wait-Stable $meta.FullName
        Start-Sleep -Milliseconds 1500
        $over = if ($Arg) { $Arg } else { '0.004' }
        python (Join-Path $PSScriptRoot 'resolves.py') $mine --against $play --over $over
    }
    'audit' {
        # The textures used in the last second (renderer/src/native/textures.cpp, Audit): the
        # ones whose guest memory changed without the cache seeing a write, then the ones whose
        # GPU copy, read back, differs from guest memory. Two summary lines when all is well.
        Get-Game $s | Out-Null
        $control = Join-Path $s.live 'control.txt'
        $start = Get-Date
        Add-Content -Path $control -Value 'set rexnative_texture_audit 2' -Encoding ascii
        Start-Sleep -Milliseconds 1500
        Add-Content -Path $control -Value 'set rexnative_texture_audit 3' -Encoding ascii
        Start-Sleep -Milliseconds 2500
        Get-Content -LiteralPath (Get-LogPath $s) -Tail 2000 | Select-String 'ReXNative texture audit' |
            ForEach-Object { $_.Line } | Where-Object {
                $t = [datetime]::MinValue
                [datetime]::TryParse($_.Substring(0, 23), [ref]$t) -and $t -ge $start.AddSeconds(-1)
            } | ForEach-Object { $_ -replace '^.*ReXNative texture audit: ', '' }
    }
    'idle' {
        # The resolved images nothing was resolved into for N frames (default 60) are dropped
        # (--rexnative_resolve_drop_idle): the textures over their memory take the guest's bytes.
        Get-Game $s | Out-Null
        $n = if ($Arg -match '^\d+$') { $Arg } else { '60' }
        $start = Get-Date
        Add-Content -Path (Join-Path $s.live 'control.txt') -Value "set rexnative_resolve_drop_idle $n" -Encoding ascii
        Start-Sleep -Milliseconds 2500
        Get-Content -LiteralPath (Get-LogPath $s) -Tail 400 | Select-String 'ReXNative resolve: (idle image|[0-9]+ images idle)' |
            ForEach-Object { $_.Line } | Where-Object {
                $t = [datetime]::MinValue
                [datetime]::TryParse($_.Substring(0, 23), [ref]$t) -and $t -ge $start.AddSeconds(-1)
            } | ForEach-Object { $_ -replace '^.*ReXNative resolve: ', '' }
    }
    'oracle' {
        # Every resolve of the next frame, ReXNative's against the Xenos plugin's (start -Oracle):
        # only the ones whose mean difference is above the threshold (default 0.02) are printed.
        Get-Game $s | Out-Null
        if (-not $s.oracle) { throw 'the live game was not started with -Oracle' }
        $dir = Join-Path $s.live 'resolves'
        $start = Get-Date
        New-Item -ItemType File -Force (Join-Path $dir 'dump.request') | Out-Null
        $meta = $null
        for ($i = 0; $i -lt 100 -and -not $meta; ++$i) {
            Start-Sleep -Milliseconds 300
            $meta = Get-ChildItem $dir -Filter 'f*-resolves.jsonl' | Where-Object { $_.LastWriteTime -ge $start } |
                Sort-Object LastWriteTime | Select-Object -Last 1
        }
        if (-not $meta) { throw 'no resolve dump within 30 s' }
        Wait-Stable $meta.FullName
        Start-Sleep -Milliseconds 1500  # the frame's .bin files
        $frame = [regex]::Match($meta.Name, '^f(\d+)-').Groups[1].Value
        $over = if ($Arg) { $Arg } else { '0.02' }
        python (Join-Path $PSScriptRoot 'resolves.py') $dir --frame $frame --over $over
    }
    'log' {
        if (-not $s) { throw 'no live game' }
        $path = Get-LogPath $s
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
        # stop -Dir <folder> stops only the game started from that folder: the live game is one
        # for the whole PC, and another session's (or the app's boot test) is left running.
        if ($s -and $Dir -and ([IO.Path]::GetFullPath($Dir).TrimEnd('\') -ne [IO.Path]::GetFullPath($s.dir).TrimEnd('\'))) {
            "not stopped: the live game is $($s.exe) (pid $($s.pid)), not one in $Dir"; break
        }
        if ($s) {
            # Its exit can take seconds (the D3D12 device's teardown): waited for, so that a
            # rebuild can replace the executable right after.
            $p = Get-Process -Id $s.pid -ErrorAction SilentlyContinue
            if ($p) { Stop-Process -Id $s.pid -Force -ErrorAction SilentlyContinue; $p.WaitForExit(20000) | Out-Null }
            Remove-Item $statePath -ErrorAction SilentlyContinue
        }
        'stopped'
    }
    default { throw "unknown command '$Command' (start, play, compare, audit, idle, keys, click, shot, frame, burst, debug, snap, drawlog, control, trace, capture, record, oracle, log, status, stop)" }
}
