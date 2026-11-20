# A reference for a game ReXNative draws wrong: the same game in Xenia Canary, driven to the same
# spot with the same kind of route, for window shots and RenderDoc captures to set beside
# ReXNative's (scripts\live.ps1, scripts\rdoc.ps1). The frame-exact reference is live.ps1's
# -Oracle (ReXGlue's Xenos renderer in the same process, the same guest frame); this one is the
# independent emulator, for when the two in the process agree and both look wrong.
#
#   scripts\xenia.ps1 start <default.xex or image> [-RenderDoc] [-Route "<keys>"]
#   scripts\xenia.ps1 keys "0:X:250,4:ENTER:250"   # <seconds from now>:<key>:<ms>, in order
#   scripts\xenia.ps1 shot race                    # <xenia>\shots\race.png (window capture)
#   scripts\xenia.ps1 capture [N]                  # with -RenderDoc: the next N frames, a capture
#                                                  # each (<xenia>\rdoc\cap_frame<N>.rdc)
#   scripts\xenia.ps1 status | stop                # stop: only the Xenia this script started
# Keys are posted to Xenia's window (no focus taken): its keyboard is the pad (X = Start,
# ENTER = A, arrows = D-pad; SPACE, ESC, TAB, F1-F12, letters and digits).
# Xenia's folder (xenia_canary.exe): -XeniaDir, $env:REXNATIVE_XENIA, else a xenia_canary folder
# in a Tools folder at the root of a drive. RenderDoc's is found as scripts\rdoc.ps1 finds it.
# A Xenia capture has no ReXNative events: `rdoc.ps1 actions` lists its draws, `save`, `history`
# and `inputs` work as on any capture. Xenia draws a frame its own way (its own targets and
# resolve shaders), so the passes are matched by what they draw, not by event number.
param(
    [Parameter(Position = 0)][string]$Command = 'status',
    [Parameter(Position = 1)][string]$Arg = '',
    [string]$Route = '',
    [switch]$RenderDoc,
    [string]$XeniaDir = $env:REXNATIVE_XENIA,
    [string]$RenderDocDir = $env:REXNATIVE_RENDERDOC
)
$ErrorActionPreference = 'Stop'
$statePath = Join-Path $env:LOCALAPPDATA 'rexnative-live\xenia.json'

Add-Type -AssemblyName System.Drawing
if (-not ('RexXeniaWin' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RexXeniaWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static void Post(IntPtr h, byte vk, bool down) {
        uint scan = MapVirtualKey(vk, 0);
        long l = 1 | ((long)scan << 16) | (down ? 0L : (3L << 30));
        PostMessage(h, down ? 0x100u : 0x101u, (IntPtr)vk, (IntPtr)l);
    }
}
'@
}

function Find-Tool([string]$dir, [string]$filter, [string]$exe, [string]$what) {
    $c = @()
    if ($dir) { $c += $dir }
    $c += [IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' } | ForEach-Object {
        Get-ChildItem (Join-Path $_.Name 'Tools') -Directory -Filter $filter -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | ForEach-Object FullName }
    foreach ($d in $c) { if ($d -and (Test-Path (Join-Path $d $exe))) { return $d } }
    throw "$what not found (its folder with $exe)"
}

function Read-State {
    if (-not (Test-Path $statePath)) { return $null }
    $s = Get-Content $statePath -Raw | ConvertFrom-Json
    $p = Get-Process -Id $s.pid -ErrorAction SilentlyContinue
    if (-not $p -or $p.ProcessName -ne 'xenia_canary') { return $null }
    return $s
}

function Get-Window($s) {
    $p = Get-Process -Id $s.pid
    for ($i = 0; $i -lt 50; ++$i) {
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) { return $p.MainWindowHandle }
        Start-Sleep -Milliseconds 200
    }
    throw 'Xenia has no window'
}

function Send-Keys($s, [string]$schedule) {
    $map = @{ SPACE = 0x20; ENTER = 0x0D; UP = 0x26; DOWN = 0x28; LEFT = 0x25; RIGHT = 0x27; TAB = 0x09; ESC = 0x1B; BACKSPACE = 0x08 }
    for ($f = 1; $f -le 12; $f++) { $map["F$f"] = 0x6F + $f }
    $h = Get-Window $s
    $start = Get-Date
    foreach ($k in ($schedule -split ',' | Where-Object { $_ })) {
        $parts = $k.Split(':'); $at = [double]$parts[0]; $name = $parts[1]
        $ms = if ($parts.Count -gt 2) { [int]$parts[2] } else { 250 }
        while (((Get-Date) - $start).TotalSeconds -lt $at) { Start-Sleep -Milliseconds 100 }
        $vk = if ($map.ContainsKey($name)) { [byte]$map[$name] } else { [byte][char]$name.ToUpper() }
        [RexXeniaWin]::Post($h, $vk, $true); Start-Sleep -Milliseconds $ms; [RexXeniaWin]::Post($h, $vk, $false)
        "$name $ms ms at ${at}s"
    }
}

$s = Read-State
switch ($Command) {
    'start' {
        if ($s) { throw "Xenia is running (pid $($s.pid)): stop it first" }
        if (-not $Arg) { throw 'start needs the game: default.xex or a disc image' }
        $game = (Resolve-Path $Arg).Path
        $xenia = Find-Tool $XeniaDir 'xenia_canary*' 'xenia_canary.exe' 'Xenia Canary'
        $exe = Join-Path $xenia 'xenia_canary.exe'
        foreach ($d in 'shots', 'rdoc') { New-Item -ItemType Directory -Force (Join-Path $xenia $d) | Out-Null }
        $before = @(Get-Process xenia_canary -ErrorAction SilentlyContinue | ForEach-Object Id)
        if ($RenderDoc) {
            # Xenia takes RenderDoc only when it is started from it.
            $rd = Find-Tool $RenderDocDir 'RenderDoc*' 'renderdoccmd.exe' 'RenderDoc'
            Start-Process (Join-Path $rd 'renderdoccmd.exe') -WindowStyle Hidden -WorkingDirectory $xenia -ArgumentList @(
                'capture', '-d', "`"$xenia`"", '-c', "`"$xenia\rdoc\cap`"", "`"$exe`"", "`"$game`"") | Out-Null
        } else {
            Start-Process $exe -ArgumentList "`"$game`"" -WorkingDirectory $xenia | Out-Null
        }
        $p = $null
        for ($i = 0; $i -lt 100 -and -not $p; ++$i) {
            Start-Sleep -Milliseconds 200
            $p = Get-Process xenia_canary -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | Select-Object -First 1
        }
        if (-not $p) { throw 'Xenia did not start' }
        New-Item -ItemType Directory -Force (Split-Path $statePath) | Out-Null
        $s = [pscustomobject]@{ pid = $p.Id; dir = $xenia; game = $game; renderdoc = [bool]$RenderDoc }
        $s | ConvertTo-Json | Set-Content $statePath -Encoding utf8
        "started pid $($p.Id): $game"
        if ($Route) { Send-Keys $s $Route; 'route done' }
    }
    'keys' { if (-not $s) { throw 'Xenia is not running' }; Send-Keys $s $Arg }
    'shot' {
        if (-not $s) { throw 'Xenia is not running' }
        $h = Get-Window $s
        $r = New-Object RexXeniaWin+RECT; [RexXeniaWin]::GetWindowRect($h, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [RexXeniaWin]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc)
        $name = if ($Arg) { $Arg } else { (Get-Date).ToString('HHmmss') }
        $path = Join-Path $s.dir "shots\$name.png"
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
        $path
    }
    'capture' {
        # Through RenderDoc's target control (scripts\rdoc_trigger.py in qrenderdoc's Python).
        if (-not $s) { throw 'Xenia is not running' }
        if (-not $s.renderdoc) { throw 'Xenia was not started with -RenderDoc' }
        $rd = Find-Tool $RenderDocDir 'RenderDoc*' 'qrenderdoc.exe' 'RenderDoc'
        $log = Join-Path $s.dir 'rdoc\trigger.log'
        Remove-Item $log -ErrorAction SilentlyContinue
        $env:REXNATIVE_RDOC_TRIGGER_OUT = $log
        $env:REXNATIVE_RDOC_FRAMES = if ($Arg -match '^\d+$') { $Arg } else { '1' }
        $q = Start-Process (Join-Path $rd 'qrenderdoc.exe') -ArgumentList @('--python', "`"$PSScriptRoot\rdoc_trigger.py`"") -PassThru -WindowStyle Minimized
        if (-not $q.WaitForExit(90000)) { Stop-Process $q -Force; throw 'RenderDoc did not answer in 90 s' }
        $text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
        $ms = [regex]::Matches($text, '(?m)^capture (.+?)\s*$')
        if ($ms.Count -eq 0) { throw "no capture: $($text.Trim())" }
        foreach ($m in $ms) { $m.Groups[1].Value }
    }
    'status' { if ($s) { "running: pid $($s.pid), $($s.game)" } else { 'not running' } }
    'stop' {
        if ($s) {
            $p = Get-Process -Id $s.pid -ErrorAction SilentlyContinue
            if ($p) { Stop-Process -Id $s.pid -Force -ErrorAction SilentlyContinue; $p.WaitForExit(15000) | Out-Null }
        }
        Remove-Item $statePath -ErrorAction SilentlyContinue
        'stopped'
    }
    default { throw "unknown command '$Command' (start, keys, shot, capture, status, stop)" }
}
