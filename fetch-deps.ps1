$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$cache = Join-Path $root 'work/pdfium-bootstrap'
New-Item $cache -ItemType Directory -Force | Out-Null
$archive = Join-Path $cache 'pdfium-win-x64.tgz'
$url = 'https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8076/pdfium-win-x64.tgz'
if (!(Test-Path -LiteralPath $archive)) { Invoke-WebRequest $url -OutFile $archive }
$expected = '808d36da9bc5a3104315fb307c80998121f565ee53953633bf33e80d7429e5ac'
if ((Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant() -ne $expected) { throw 'PDFium SHA256 mismatch; download rejected.' }
& tar.exe -xzf $archive -C $cache
if ($LASTEXITCODE -ne 0) { throw 'Could not extract PDFium.' }
New-Item (Join-Path $root 'vendor/pdfium/lib') -ItemType Directory -Force | Out-Null
Copy-Item (Join-Path $cache 'lib/pdfium.dll.lib') (Join-Path $root 'vendor/pdfium/lib/pdfium.dll.lib') -Force
Copy-Item (Join-Path $cache 'bin/pdfium.dll') (Join-Path $root 'pdfium.dll') -Force
Write-Output 'PDFium chromium/8076 Windows x64 verified and ready.'
