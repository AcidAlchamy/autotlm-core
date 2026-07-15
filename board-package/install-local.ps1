# Install the AutoTLM boards platform into the local Arduino sketchbook
# (Documents\Arduino\hardware\autotlm\esp32) for development — no Boards
# Manager release needed. Re-run after editing the platform.
#
# The esp32 core's build recipes resolve a few helper files relative to the
# BOARD's platform ({runtime.platform.path}/tools/...), so those files are
# copied in from the installed esp32 core rather than kept in git.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$esp32Root = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\hardware\esp32'
if (-not (Test-Path $esp32Root)) { throw 'esp32 core not installed — install esp32:esp32 (3.x) via Boards Manager first' }
$esp32 = Get-ChildItem $esp32Root -Directory | Sort-Object Name -Descending | Select-Object -First 1

$dest = Join-Path (Join-Path $env:USERPROFILE 'Documents') 'Arduino\hardware\autotlm\esp32'
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
New-Item -ItemType Directory -Force $dest | Out-Null

Copy-Item (Join-Path $here 'autotlm\esp32\*') $dest -Recurse

$tools = Join-Path $dest 'tools'
New-Item -ItemType Directory -Force $tools | Out-Null
# Merge contents (not the dir) — the repo platform now ships its own
# tools\partitions\autotlm_ota.csv and it must survive alongside the core's.
$parts = Join-Path $tools 'partitions'
New-Item -ItemType Directory -Force $parts | Out-Null
Copy-Item (Join-Path $esp32.FullName 'tools\partitions\*') $parts -Recurse
foreach ($f in 'gen_esp32part.exe', 'gen_esp32part.py', 'espota.exe', 'espota.py') {
  Copy-Item (Join-Path $esp32.FullName "tools\$f") $tools
}

Write-Host "Installed to $dest (tools from esp32 core $($esp32.Name))."
Write-Host 'Board: Tools -> Board -> AutoTLM Boards -> AutoTLM One   (FQBN autotlm:esp32:one)'
