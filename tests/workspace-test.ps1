$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public class BrisaTestWindow {
    public delegate bool WindowCallback(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumThreadWindows(
        uint threadId, WindowCallback callback, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(
        IntPtr window, StringBuilder className, int capacity);

    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(
        IntPtr window, uint message, IntPtr wordParameter, IntPtr longParameter);

    public static IntPtr FindWindow(int processId) {
        IntPtr foundWindow = IntPtr.Zero;
        var process = System.Diagnostics.Process.GetProcessById(processId);
        foreach (System.Diagnostics.ProcessThread thread in process.Threads) {
            EnumThreadWindows((uint)thread.Id, (window, parameter) => {
                var className = new StringBuilder(100);
                GetClassName(window, className, 100);
                if (className.ToString() == "BrisaPdfWindow") {
                    foundWindow = window;
                }
                return true;
            }, IntPtr.Zero);
        }
        return foundWindow;
    }
}
'@
$executable = (Resolve-Path work/Brisa-03-test.exe).Path
Copy-Item 'work/estudio ñ.pdf' 'work/Ley de referencia.pdf' -Force
Copy-Item work/segundo.pdf 'work/Ejercicios de clase.pdf' -Force
$referencePdf = (Resolve-Path 'work/Ley de referencia.pdf').Path
$exercisePdf = (Resolve-Path 'work/Ejercicios de clase.pdf').Path
# Query IDs from the BRISA_TEST-only WM_APP+10 handler.
$state = @{
    WorkspaceCount = 0
    PaneCount = 1
    LeftPage = 3
    RightPage = 4
    HighlightCount = 5
    ActiveWorkspace = 7
}

foreach ($pdfPath in @($referencePdf, $exercisePdf)) {
    if (Test-Path -LiteralPath ($pdfPath + '.brisa-ink')) {
        Remove-Item -LiteralPath ($pdfPath + '.brisa-ink')
    }
}

function Send-WindowMessage([int]$Message, [long]$WordParameter = 0, [long]$LongParameter = 0) {
    return [BrisaTestWindow]::SendMessage(
        $windowHandle, $Message, [IntPtr]$WordParameter, [IntPtr]$LongParameter
    ).ToInt64()
}

function Get-AppState([int]$QueryId) {
    return Send-WindowMessage 0x800A $QueryId
}

function Wait-ForRender {
    for ($attempt = 0; $attempt -lt 200; $attempt++) {
        if ((Send-WindowMessage 0x8009) -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 20
    }
    throw 'Rendering timed out'
}

function Send-Key([int]$KeyCode) {
    [void](Send-WindowMessage 0x100 $KeyCode)
}

function Send-Shortcut([int]$KeyCode, [int]$Modifiers = 1) {
    [void](Send-WindowMessage 0x8008 $Modifiers)
    Send-Key $KeyCode
    [void](Send-WindowMessage 0x8008 0)
}

function Send-Click([int]$x, [int]$y) {
    [void](Send-WindowMessage 0x201 1 ($x -bor ($y -shl 16)))
    [void](Send-WindowMessage 0x202 0 ($x -bor ($y -shl 16)))
}

function Save-Screenshot([string]$name) {
    [void](Send-WindowMessage 0x8007)
    $bitmap = [Drawing.Bitmap]::new((Join-Path $PSScriptRoot capture.bmp))
    $path = Join-Path $PSScriptRoot $name
    $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
    return (Get-FileHash $path).Hash
}

function Get-Glyph {
    [void](Send-WindowMessage 0x800B)
    $culture = [cultureinfo]::InvariantCulture
    return @(Get-Content work/glyphs.tsv | ForEach-Object {
            $columns = $_ -split "`t"
            [pscustomobject]@{
                Index = [int]$columns[0]
                Codepoint = [int]$columns[1]
                Left = [double]::Parse($columns[2], $culture)
                Top = [double]::Parse($columns[3], $culture)
                Right = [double]::Parse($columns[4], $culture)
                Bottom = [double]::Parse($columns[5], $culture)
            }
        })
}

function Select-Highlight([object]$firstGlyph, [object]$lastGlyph) {
    $startX = [int](($firstGlyph.Left + $firstGlyph.Right) / 2)
    $startY = [int](($firstGlyph.Top + $firstGlyph.Bottom) / 2)
    $endX = [int](($lastGlyph.Left + $lastGlyph.Right) / 2)
    $endY = [int](($lastGlyph.Top + $lastGlyph.Bottom) / 2)
    $startCoordinates = $startX -bor ($startY -shl 16)
    $endCoordinates = $endX -bor ($endY -shl 16)
    [void](Send-WindowMessage 0x201 1 $startCoordinates)
    [void](Send-WindowMessage 0x200 1 $endCoordinates)
    [void](Send-WindowMessage 0x202 0 $endCoordinates)
}

function Start-Brisa {
    $script:appProcess = Start-Process $executable -ArgumentList ('"' + $referencePdf + '" "' + $exercisePdf + '"') -WindowStyle Hidden -PassThru
    for ($attempt = 0; $attempt -lt 150; $attempt++) {
        $script:windowHandle = [BrisaTestWindow]::FindWindow($appProcess.Id)
        if ($windowHandle -ne [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 150
            Wait-ForRender
            return
        }
        Start-Sleep -Milliseconds 20
    }
    throw 'No window'
}

try {
    Start-Brisa
    if ((Get-AppState $state.WorkspaceCount) -ne 2) {
        throw 'Expected two individual tabs'
    }
    Send-Shortcut 49 # Ctrl+1
    Wait-ForRender
    [void](Send-WindowMessage 0x111 3001)
    Wait-ForRender
    if ((Get-AppState $state.WorkspaceCount) -ne 3 -or (Get-AppState $state.PaneCount) -ne 2) {
        throw 'Combined workspace missing'
    }
    Send-Key 34 # Page Down
    Wait-ForRender
    if ((Get-AppState $state.LeftPage) -ne 1 -or (Get-AppState $state.RightPage) -ne 0) {
        throw 'Left navigation affected right pane'
    }
    Send-Key 9
    Send-Key 34 # Page Down
    Wait-ForRender
    Send-Key 34 # Page Down
    Wait-ForRender
    if ((Get-AppState $state.LeftPage) -ne 1 -or (Get-AppState $state.RightPage) -ne 2) {
        throw 'Right navigation affected left pane'
    }
    Send-Shortcut 49 # Ctrl+1
    Wait-ForRender
    if ((Get-AppState $state.LeftPage) -ne 0) {
        throw 'Combined workspace changed individual workspace position'
    }
    Send-Shortcut 51 # Ctrl+3
    Wait-ForRender
    if ((Get-AppState $state.LeftPage) -ne 1 -or (Get-AppState $state.RightPage) -ne 2) {
        throw 'Combined workspace state lost'
    }
    Send-Shortcut 66 # Ctrl+B: tools
    Wait-ForRender # Sidebar
    Send-Key 9 # Focus left
    Send-Key 72 # Text highlighter
    $glyphs = Get-Glyph
    if ($glyphs.Count -lt 50) {
        throw 'No PDF text geometry extracted'
    }
    $cleanImageHash = Save-Screenshot 'workspace-clean.png'
    Select-Highlight $glyphs[4] $glyphs[14]
    if ((Get-AppState $state.HighlightCount) -ne 1) {
        throw 'Single-line highlight did not snap to text'
    }
    $highlightedImageHash = Save-Screenshot 'workspace-highlight.png'
    if ($cleanImageHash -eq $highlightedImageHash) {
        throw 'Text highlight invisible'
    }
    Select-Highlight $glyphs[32] $glyphs[130]
    if ((Get-AppState $state.HighlightCount) -lt 3) {
        throw 'Multi-line selection did not create line rectangles'
    }
    $appProcess.Refresh()
    "UI working set with two PDFs: $($appProcess.WorkingSet64) bytes"
    [void](Save-Screenshot 'workspace-double.png')
    Send-Shortcut 90 # Undo highlighter
    if ((Get-AppState $state.HighlightCount) -ne 1) {
        throw 'Undo did not remove the multi-line highlight as one action'
    }
    $restoredImageHash = Save-Screenshot 'workspace-restored.png'
    if ($restoredImageHash -ne $highlightedImageHash) {
        throw 'Undo failed to restore exact selection'
    }
    Send-Key 9 # Right pane must have no marks
    if ((Get-AppState $state.HighlightCount) -ne 0) {
        throw 'Highlights leaked into the other PDF'
    }
    Send-Key 9
    Send-Key 80 # Left pen
    Send-Click 280 320
    $originalHash = (Get-FileHash -LiteralPath $referencePdf).Hash
    [void](Send-WindowMessage 0x800C)
    for ($attempt = 0; $attempt -lt 250 -and (Send-WindowMessage 0x800D) -ne 0; $attempt++) {
        Start-Sleep -Milliseconds 20
    }
    if ((Send-WindowMessage 0x800D) -ne 0 -or !(Test-Path -LiteralPath ($referencePdf + '.exported.pdf'))) {
        throw 'Asynchronous PDF export failed'
    }
    if ((Get-FileHash -LiteralPath $referencePdf).Hash -ne $originalHash) {
        throw 'Export modified original PDF'
    }
    Send-Key 72
    Send-Shortcut 49 # Ctrl+1
    Wait-ForRender
    Send-Key 72
    if ((Get-AppState $state.HighlightCount) -ne 1) {
        throw 'Document highlight missing in individual workspace'
    }
    [void](Send-WindowMessage 0x10)
    $appProcess.WaitForExit()
    Start-Brisa
    Send-Shortcut 49 # Ctrl+1
    Wait-ForRender
    if ((Get-AppState $state.HighlightCount) -ne 1) {
        throw 'Text highlight not restored after restart'
    }
    Send-Key 34 # Page Down
    Wait-ForRender
    Send-Key 72
    Send-Shortcut 90
    if ((Get-AppState $state.HighlightCount) -ne 0) {
        throw 'Undo text highlight failed after restart'
    }
    $glyphs = Get-Glyph
    Select-Highlight $glyphs[14] $glyphs[4]
    if ((Get-AppState $state.HighlightCount) -ne 1) {
        throw 'Reverse text selection failed'
    }
    Send-Key 86
    Send-Shortcut 90
    if ((Get-AppState $state.HighlightCount) -ne 0) {
        throw 'Undo in reading mode did not remove last text selection'
    }
    Send-Key 72
    [void](Send-WindowMessage 0x201 1 (300 -bor (140 -shl 16)))
    [void](Send-WindowMessage 0x200 1 (500 -bor (140 -shl 16)))
    [void](Send-WindowMessage 0x202 0 (500 -bor (140 -shl 16)))
    if ((Get-AppState $state.HighlightCount) -ne 0) {
        throw 'Blank space created a freehand highlight'
    }
    # Click the second tab, close it, and keep the first PDF usable.
    Send-Click 350 22
    Wait-ForRender
    if ((Get-AppState $state.ActiveWorkspace) -ne 1) {
        throw 'Tab click failed'
    }
    Send-Shortcut 87 # Ctrl+W
    Wait-ForRender
    if ((Get-AppState $state.WorkspaceCount) -ne 1) {
        throw 'Closing a workspace failed'
    }
    Send-Shortcut 87 # Ctrl+W
    Start-Sleep -Milliseconds 200
    if ((Get-AppState $state.WorkspaceCount) -ne 0) {
        throw 'Closing last workspace failed'
    }
    $file = [IO.File]::Open($referencePdf, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $file.Dispose()
    "PASS: tabs, combined workspace, independent navigation, workspace shortcuts, sidebar, text snapping, multiline/reverse selection, grouped undo, no freehand highlights, separate marks, annotation restart, tab click/close, releasing last PDF"
}
finally {
    if ($windowHandle -and $windowHandle -ne [IntPtr]::Zero) {
        [void](Send-WindowMessage 0x10)
    }
    if ($appProcess -and !$appProcess.HasExited) {
        [void]$appProcess.WaitForExit(5000)
    }
}
