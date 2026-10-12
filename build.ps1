param([string]$Zig = 'zig')

$ErrorActionPreference = 'Stop'
$compilerArguments = @(
    'c++'
    'src/main.cpp'
    '-o', 'Brisa.exe'
    '-I', 'vendor/pdfium/include'
    'vendor/pdfium/lib/pdfium.dll.lib'
    '-target', 'x86_64-windows-gnu'
    '-std=c++17'
    '-O2'
    '-s'
    '-municode'
    '-Wl,--subsystem,windows'
    '-luser32'
    '-lgdi32'
    '-lcomdlg32'
    '-lshell32'
    '-lpsapi'
    '-lgdiplus'
)

Push-Location $PSScriptRoot
try {
    & $Zig @compilerArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'Error de compilación'
    }
}
finally {
    Pop-Location
}
