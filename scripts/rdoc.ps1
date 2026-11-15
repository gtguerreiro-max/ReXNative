# Queries a RenderDoc capture with scripts/rdoc.py (RenderDoc's own Python, no window kept).
#   scripts\rdoc.ps1 actions <capture.rdc>
#   scripts\rdoc.ps1 history <capture.rdc> -X 400 -Y 120 [-Event 0] [-Texture 0]
#   scripts\rdoc.ps1 save <capture.rdc> -Event 1234 [-Texture 0]
#   scripts\rdoc.ps1 inputs <capture.rdc> -Event 1234      # its textures (as PNG) and targets
# RenderDoc's folder: -RenderDoc, else $env:REXNATIVE_RENDERDOC (the one with qrenderdoc.exe).
# Output: <capture>.rdoc\ (actions.txt, history-X-Y.txt, tex-*.png), printed when done.
param(
    [Parameter(Position = 0, Mandatory)][ValidateSet('actions', 'history', 'save', 'inputs')][string]$Cmd,
    [Parameter(Position = 1, Mandatory)][string]$Capture,
    [int]$X = 0, [int]$Y = 0, [int]$Event = 0, [int]$Texture = 0,
    [string]$RenderDoc = $env:REXNATIVE_RENDERDOC
)
$ErrorActionPreference = 'Stop'
if (-not $RenderDoc -or -not (Test-Path (Join-Path $RenderDoc 'qrenderdoc.exe'))) {
    throw 'RenderDoc not found: pass -RenderDoc <folder with qrenderdoc.exe> or set REXNATIVE_RENDERDOC'
}
$capture = (Resolve-Path $Capture).Path
$out = "$capture.rdoc"
New-Item -ItemType Directory -Force $out | Out-Null
$job = Join-Path $out 'job.json'
@{ capture = $capture; out = $out; cmd = $Cmd; x = $X; y = $Y; event = $Event; texture = $Texture } |
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
    'actions' { Join-Path $out 'actions.txt' }
    'history' { Join-Path $out "history-$X-$Y.txt" }
    'save' { Get-Content (Join-Path $out 'save.txt') }
    'inputs' { Join-Path $out "inputs-e$Event.txt" }
}
