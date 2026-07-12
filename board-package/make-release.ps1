# Package the AutoTLM boards platform for a Boards-Manager release.
#
# Produces autotlm-boards-<version>.zip next to this script and prints the
# checksum + size to paste into package_autotlm_index.json. Publish the zip
# as a GitHub release asset named boards-<version> so the index URL resolves.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$platformTxt = Get-Content (Join-Path $here 'autotlm\esp32\platform.txt') -Raw
if ($platformTxt -notmatch 'version=(\S+)') { throw 'No version= in platform.txt' }
$version = $Matches[1]

$zipName = "autotlm-boards-$version.zip"
$zipPath = Join-Path $here $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath }

# The archive must contain a single root folder; Boards Manager strips it.
$staging = Join-Path $env:TEMP "autotlm-boards-$version"
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
Copy-Item (Join-Path $here 'autotlm\esp32') $staging -Recurse

# The esp32 core's recipes resolve these relative to the BOARD's platform
# ({runtime.platform.path}/tools/...), so the release must carry them. They
# are pulled from the locally installed esp32 core, not kept in git.
$esp32Root = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\hardware\esp32'
if (-not (Test-Path $esp32Root)) { throw 'esp32 core not installed — install esp32:esp32 (3.x) via Boards Manager first' }
$esp32 = Get-ChildItem $esp32Root -Directory | Sort-Object Name -Descending | Select-Object -First 1
$tools = Join-Path $staging 'tools'
New-Item -ItemType Directory -Force $tools | Out-Null
Copy-Item (Join-Path $esp32.FullName 'tools\partitions') $tools -Recurse
foreach ($f in 'gen_esp32part.exe', 'gen_esp32part.py', 'espota.exe', 'espota.py') {
  Copy-Item (Join-Path $esp32.FullName "tools\$f") $tools
}

Compress-Archive -Path $staging -DestinationPath $zipPath
Remove-Item $staging -Recurse -Force

$hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item $zipPath).Length

Write-Host "Built  : $zipPath"
Write-Host "Paste into package_autotlm_index.json:"
Write-Host "  ""version"": ""$version"","
Write-Host "  ""archiveFileName"": ""$zipName"","
Write-Host "  ""checksum"": ""SHA-256:$hash"","
Write-Host "  ""size"": ""$size"","
