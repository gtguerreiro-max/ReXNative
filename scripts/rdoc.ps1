# Queries a RenderDoc capture with scripts/rdoc.py (RenderDoc's own Python, no window kept).
#   scripts\rdoc.ps1 summary <capture.rdc>                 # the frame in a few lines: passes (targets,
#                    draw count), clears, resolves, Swap, printed; start here
#   scripts\rdoc.ps1 draws <capture.rdc> -Event 132        # a pass's draws (index, shaders), printed
#   scripts\rdoc.ps1 find <capture.rdc> -Text 1954F000     # the events named with a text, printed
#   scripts\rdoc.ps1 actions <capture.rdc>                 # every action (long)
#   scripts\rdoc.ps1 history <capture.rdc> -X 400 -Y 120 [-X1 410] [-Event 0] [-Texture 0]
#   scripts\rdoc.ps1 save <capture.rdc> -Event 1234 [-Texture 0] [-X <mip level>]
#   scripts\rdoc.ps1 inputs <capture.rdc> -Event 1234      # its textures (as PNG) and targets
#   scripts\rdoc.ps1 reads <capture.rdc> [-Event 1234]     # every draw from there: what its pixel
#                    shader reads and where it draws, one line each (reads-e<event>.txt)
#   scripts\rdoc.ps1 pick <capture.rdc> -Event 1234 -Texture 56 [-X 10 -X1 20 -Y 5]
#                    float values of a texture after an event at a few points (pick-e<event>-<tex>.txt)
#   scripts\rdoc.ps1 debug <capture.rdc> -Event 1234 -X 560 -Y 600   # its pixel shader at a pixel
#                    in RenderDoc's debugger: inputs, then each instruction's results
#   scripts\rdoc.ps1 mesh <capture.rdc> -Event 1234         # its vertex shader's output positions
#   scripts\rdoc.ps1 disasm <capture.rdc> -Event 1234       # its shaders, disassembled
#   scripts\rdoc.ps1 cbuffers <capture.rdc> -Event 1234     # its shaders' constant buffers
#   scripts\rdoc.ps1 state <capture.rdc> -Event 1234        # its depth, stencil, bias, viewport
#   scripts\rdoc.ps1 dump <capture.rdc> -Event 1234 -Texture 56   # its mip 0 as raw texels
#   scripts\rdoc.ps1 buffer <capture.rdc> -Offset 15494000 -Size 1000 [-Event 0] [-Texture 0]
#                    bytes (hex offset and size) of a buffer; 0: the largest, the renderer's
#                    copy of guest physical memory
# RenderDoc's folder: -RenderDoc, else $env:REXNATIVE_RENDERDOC (the one with qrenderdoc.exe).
# Output: <capture>.rdoc\ (actions.txt, history-X-Y.txt, tex-*.png), printed when done.
param(
    [Parameter(Position = 0, Mandatory)][ValidateSet('summary', 'draws', 'find', 'actions', 'history', 'save', 'inputs', 'reads', 'pick', 'debug', 'buffer', 'mesh', 'disasm', 'cbuffers', 'state', 'dump')][string]$Cmd,
    [Parameter(Position = 1, Mandatory)][string]$Capture,
    [int]$X = 0, [int]$X1 = 0, [int]$Y = 0, [int]$Event = 0, [int]$Texture = 0, [string]$Offset = '0', [string]$Size = '0', [string]$Text = '',
    [string]$RenderDoc = $env:REXNATIVE_RENDERDOC
)
$ErrorActionPreference = 'Stop'
function Find-RenderDoc([string]$dir) {
    # -RenderDoc / REXNATIVE_RENDERDOC, else the newest RenderDoc* in a Tools folder at the root
    # of a drive (F:\Tools\RenderDoc_1.47_64), else where its installer puts it: as the renderer's
    # --rexnative_renderdoc=auto looks for it.
    $c = @()
    if ($dir) { $c += $dir }
    $c += [IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' } | ForEach-Object {
        Get-ChildItem (Join-Path $_.Name 'Tools') -Directory -Filter 'RenderDoc*' -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | ForEach-Object FullName }
    $c += Join-Path $env:ProgramFiles 'RenderDoc'
    foreach ($d in $c) { if ($d -and (Test-Path (Join-Path $d 'qrenderdoc.exe'))) { return $d } }
    throw 'RenderDoc not found: -RenderDoc <folder>, REXNATIVE_RENDERDOC, <drive>:\Tools\RenderDoc* or Program Files\RenderDoc'
}
$RenderDoc = Find-RenderDoc $RenderDoc
$capture = (Resolve-Path $Capture).Path
$out = "$capture.rdoc"
New-Item -ItemType Directory -Force $out | Out-Null
$job = Join-Path $out 'job.json'
@{ capture = $capture; out = $out; cmd = $Cmd; x = $X; x1 = $X1; y = $Y; event = $Event; texture = $Texture; offset = $Offset; size = $Size; text = $Text } |
    ConvertTo-Json | Set-Content $job -Encoding utf8
Remove-Item (Join-Path $out 'rdoc.log') -ErrorAction SilentlyContinue
$env:REXNATIVE_RDOC_JOB = $job
$p = Start-Process (Join-Path $RenderDoc 'qrenderdoc.exe') -ArgumentList @('--python', "`"$PSScriptRoot\rdoc.py`"") -PassThru -WindowStyle Minimized
if (-not $p.WaitForExit(600000)) { Stop-Process $p -Force; throw 'RenderDoc did not finish in 10 minutes' }
$log = Join-Path $out 'rdoc.log'
if (-not (Test-Path $log)) { throw 'rdoc.py wrote no log' }
$text = Get-Content $log -Raw
if ($text.Trim() -ne 'ok') { throw "rdoc.py failed:`n$text" }
switch ($Cmd) {
    { $_ -in 'summary', 'draws', 'find' } { Get-Content (Join-Path $out "$Cmd.txt") }
    'actions' { Join-Path $out 'actions.txt' }
    'history' { Join-Path $out "history-$X-$Y.txt" }
    'save' { Get-Content (Join-Path $out 'save.txt') }
    'inputs' { Join-Path $out "inputs-e$Event.txt" }
    'reads' { Join-Path $out "reads-e$Event.txt" }
    'pick' { Join-Path $out "pick-e$Event-$Texture.txt" }
    'debug' { Join-Path $out "debug-e$Event-$X-$Y.txt" }
    'buffer' { Get-Content (Join-Path $out 'buffer.txt') }
    'mesh' { Join-Path $out "mesh-e$Event.txt" }
    'disasm' { Join-Path $out "disasm-e$Event-ps.txt" }
    'cbuffers' { Join-Path $out "cbuffers-e$Event.txt" }
    'state' { Get-Content (Join-Path $out "state-e$Event.txt") }
    'dump' { Get-Content (Join-Path $out 'dump.txt') }
}
