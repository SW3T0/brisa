$ErrorActionPreference = 'Stop'
$projectDirectory = $PSScriptRoot
$cacheDirectory = Join-Path $projectDirectory 'work/pdfium-bootstrap'
$archivePath = Join-Path $cacheDirectory 'pdfium-win-x64.tgz'
$downloadUrl = 'https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8076/pdfium-win-x64.tgz'
$expectedHash = '808d36da9bc5a3104315fb307c80998121f565ee53953633bf33e80d7429e5ac'

New-Item $cacheDirectory -ItemType Directory -Force | Out-Null
if (!(Test-Path -LiteralPath $archivePath)) {
    Invoke-WebRequest $downloadUrl -OutFile $archivePath
}
if ((Get-FileHash -LiteralPath $archivePath).Hash.ToLowerInvariant() -ne $expectedHash) {
    throw 'PDFium SHA256 mismatch; download rejected.'
}

& tar.exe -xzf $archivePath -C $cacheDirectory
if ($LASTEXITCODE -ne 0) {
    throw 'Could not extract PDFium.'
}

$libraryDirectory = Join-Path $projectDirectory 'vendor/pdfium/lib'
$importLibraryPath = Join-Path $libraryDirectory 'pdfium.dll.lib'
$runtimeLibraryPath = Join-Path $projectDirectory 'pdfium.dll'
New-Item $libraryDirectory -ItemType Directory -Force | Out-Null
Copy-Item (Join-Path $cacheDirectory 'lib/pdfium.dll.lib') $importLibraryPath -Force
Copy-Item (Join-Path $cacheDirectory 'bin/pdfium.dll') $runtimeLibraryPath -Force
Write-Output 'PDFium chromium/8076 Windows x64 verified and ready.'
