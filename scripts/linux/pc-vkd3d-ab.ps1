# Runs a game on the PC once with Windows' own D3D12 and once through vkd3d-proton, to see what
# vkd3d-proton (the Steam Deck's D3D12 under Proton) costs ReXNative's draw path on the same CPU.
#   scripts\linux\pc-vkd3d-ab.ps1 -Dir <a COPY of the game's run folder> -Exe <name>.exe
#       -Vkd3d <folder with vkd3d-proton's d3d12.dll and d3d12core.dll and DXVK's dxgi.dll>
#       [-Mode native|vkd3d] [-Seconds 85] [-From 40] [-Heavy 800] [-Label <name>]
# vkd3d-proton's Windows DLLs (a Proton's files/lib/wine/vkd3d-proton/x86_64-windows and
# dxvk/x86_64-windows, the Deck's own GE-Proton for its exact version) go beside the executable
# for the run and are removed after it: use a copy of the run folder, never the game's own.
# Prints the means of ReXNative's frame lines with at least -Heavy draws, the process's CPU time
# from -From seconds to the end (% of one core), and where d3d12.dll and dxgi.dll were loaded from.
# Alternate the modes (native, vkd3d, native, vkd3d): another program on the PC adds noise.
param([Parameter(Mandatory)] [string]$Dir, [Parameter(Mandatory)] [string]$Exe,
      [Parameter(Mandatory)] [string]$Vkd3d, [ValidateSet('native', 'vkd3d')] [string]$Mode = 'native',
      [int]$Seconds = 85, [int]$From = 40, [int]$Heavy = 800, [string]$Label = $Mode)
$ErrorActionPreference = 'Stop'
$dlls = 'd3d12.dll', 'd3d12core.dll', 'dxgi.dll'
foreach ($f in $dlls) { if (Test-Path "$Dir\$f") { Remove-Item "$Dir\$f" -Force } }
if ($Mode -eq 'vkd3d') { foreach ($f in $dlls) { Copy-Item "$Vkd3d\$f" "$Dir\$f" } }
$log = "ab-$Label"
$gameArgs = "--log_file=$log", '--rexnative_d3d_stats_frames=60', '--rexnative_upscaler=false',
            '--fullscreen=false', '--window_width=1280', '--window_height=720'
$p = Start-Process -FilePath "$Dir\$Exe" -ArgumentList $gameArgs -WorkingDirectory $Dir -PassThru
try {
  Start-Sleep -Seconds $From
  $c0 = (Get-Process -Id $p.Id).TotalProcessorTime.TotalMilliseconds; $t0 = Get-Date
  Start-Sleep -Seconds ($Seconds - $From)
  $c1 = (Get-Process -Id $p.Id).TotalProcessorTime.TotalMilliseconds; $t1 = Get-Date
  $mods = (Get-Process -Id $p.Id).Modules | Where-Object { $_.ModuleName -in $dlls } |
    ForEach-Object { $_.FileName }
} finally {
  Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 2
  foreach ($f in $dlls) { if (Test-Path "$Dir\$f") { Remove-Item "$Dir\$f" -Force } }
}
$cpu = ($c1 - $c0) / ($t1 - $t0).TotalMilliseconds * 100
$file = Get-ChildItem "$Dir\logs\$log*.log" | Sort-Object LastWriteTime | Select-Object -Last 1
$pattern = 'ReXNative: frames (\d+) \| per frame: draws ([\d.]+), draw CPU ([\d.]+) us .*?swap ([\d.]+) us, ' +
           'frame ([\d.]+) ms, game thread ([\d.]+) us'
$rows = Select-String -Path $file.FullName -Pattern $pattern | ForEach-Object {
  $g = $_.Matches[0].Groups
  [pscustomobject]@{ draws = [double]$g[2].Value; cpu = [double]$g[3].Value; swap = [double]$g[4].Value
                     frame = [double]$g[5].Value; game = [double]$g[6].Value }
} | Where-Object { $_.draws -ge $Heavy }
if (-not $rows) { "$Label ($Mode): no frame lines with $Heavy draws or more in $($file.Name)"; return }
$m = $rows | Measure-Object draws, cpu, swap, frame, game -Average
'{0,-10} {1,-6} lines {2,2}  draws {3,6:N0}  draw CPU {4,6:N0} us  {5,5:N3} us/draw  swap {6,4:N0} us  frame {7,5:N2} ms  game thread {8,4:N0} us  process CPU {9,4:N0} %' -f `
  $Label, $Mode, @($rows).Count, $m[0].Average, $m[1].Average, ($m[1].Average / $m[0].Average),
  $m[2].Average, $m[3].Average, $m[4].Average, $cpu
"  loaded: $($mods -join ', ')"
