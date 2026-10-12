$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public class Brisa03 {
 public delegate bool Callback(IntPtr h,IntPtr p);
 [DllImport("user32.dll")] static extern bool EnumThreadWindows(uint id,Callback cb,IntPtr p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint msg,IntPtr w,IntPtr l);
 public static IntPtr Window(int pid) {IntPtr found=IntPtr.Zero;foreach(System.Diagnostics.ProcessThread t in System.Diagnostics.Process.GetProcessById(pid).Threads)EnumThreadWindows((uint)t.Id,(h,p)=>{var s=new StringBuilder(100);GetClassName(h,s,100);if(s.ToString()=="BrisaPdfWindow")found=h;return true;},IntPtr.Zero);return found;}
}
'@
$exe=(Resolve-Path work/Brisa-03-test.exe).Path
Copy-Item 'work/estudio ñ.pdf' 'work/Ley de referencia.pdf' -Force
Copy-Item work/segundo.pdf 'work/Ejercicios de clase.pdf' -Force
$pdf=(Resolve-Path 'work/Ley de referencia.pdf').Path
$pdf2=(Resolve-Path 'work/Ejercicios de clase.pdf').Path
foreach($f in @($pdf,$pdf2)) {if(Test-Path -LiteralPath ($f+'.brisa-ink')) {Remove-Item -LiteralPath ($f+'.brisa-ink')}}
function Send([int]$m,[long]$w=0,[long]$l=0) {return [Brisa03]::SendMessage($h,$m,[IntPtr]$w,[IntPtr]$l).ToInt64()}
function Query([int]$kind) {return Send 0x800A $kind}
function Wait {
 for($n=0;$n -lt 200;$n++) {if((Send 0x8009) -eq 0) {return};Start-Sleep -Milliseconds 20};throw 'Rendering timed out'
}
function Key([int]$k) {[void](Send 0x100 $k)}
function Shortcut([int]$k,[int]$mod=1) {[void](Send 0x8008 $mod);Key $k;[void](Send 0x8008 0)}
function Click([int]$x,[int]$y) {[void](Send 0x201 1 ($x -bor ($y -shl 16)));[void](Send 0x202 0 ($x -bor ($y -shl 16)))}
function Shot([string]$name) {
 [void](Send 0x8007);$b=[Drawing.Bitmap]::new((Join-Path $PSScriptRoot capture.bmp));$path=Join-Path $PSScriptRoot $name;$b.Save($path,[Drawing.Imaging.ImageFormat]::Png);$b.Dispose();return (Get-FileHash $path).Hash
}
function Glyphs {
 [void](Send 0x800B)
 return @(Get-Content work/glyphs.tsv | ForEach-Object {$v=$_ -split "`t";[pscustomobject]@{Index=[int]$v[0];Code=[int]$v[1];L=[double]::Parse($v[2],[cultureinfo]::InvariantCulture);T=[double]::Parse($v[3],[cultureinfo]::InvariantCulture);R=[double]::Parse($v[4],[cultureinfo]::InvariantCulture);B=[double]::Parse($v[5],[cultureinfo]::InvariantCulture)}})
}
function HighlightText([object]$first,[object]$last) {
 $x=[int](($first.L+$first.R)/2);$y=[int](($first.T+$first.B)/2);$endX=[int](($last.L+$last.R)/2);$endY=[int](($last.T+$last.B)/2)
 [void](Send 0x201 1 ($x -bor ($y -shl 16)))
 [void](Send 0x200 1 ($endX -bor ($endY -shl 16)))
 [void](Send 0x202 0 ($endX -bor ($endY -shl 16)))
}
function Launch {
 $script:p=Start-Process $exe -ArgumentList ('"'+$pdf+'" "'+$pdf2+'"') -WindowStyle Hidden -PassThru
 for($n=0;$n -lt 150;$n++) {$script:h=[Brisa03]::Window($p.Id);if($h -ne [IntPtr]::Zero) {Start-Sleep -Milliseconds 150;Wait;return};Start-Sleep -Milliseconds 20};throw 'No window'
}
try {
 Launch
 if((Query 0) -ne 2) {throw 'Expected two individual tabs'}
 Shortcut 49;Wait
 [void](Send 0x111 3001);Wait
 if((Query 0) -ne 3 -or (Query 1) -ne 2) {throw 'Combined workspace missing'}
 Key 34;Wait
 if((Query 3) -ne 1 -or (Query 4) -ne 0) {throw 'Left navigation affected right pane'}
 Key 9;Key 34;Wait;Key 34;Wait
 if((Query 3) -ne 1 -or (Query 4) -ne 2) {throw 'Right navigation affected left pane'}
 Shortcut 49;Wait
 if((Query 3) -ne 0) {throw 'Combined workspace changed individual workspace position'}
 Shortcut 51;Wait
 if((Query 3) -ne 1 -or (Query 4) -ne 2) {throw 'Combined workspace state lost'}
 Shortcut 66;Wait # Sidebar
 Key 9 # Focus left
 Key 72 # Text highlighter
 $glyphs=Glyphs
 if($glyphs.Count -lt 50) {throw 'No PDF text geometry extracted'}
 $clean=Shot 'workspace-clean.png'
 HighlightText $glyphs[4] $glyphs[14]
 if((Query 5) -ne 1) {throw 'Single-line highlight did not snap to text'}
 $marked=Shot 'workspace-highlight.png'
 if($clean -eq $marked) {throw 'Text highlight invisible'}
 HighlightText $glyphs[32] $glyphs[130]
 if((Query 5) -lt 3) {throw 'Multi-line selection did not create line rectangles'}
 $p.Refresh(); "UI working set with two PDFs: $($p.WorkingSet64) bytes"
 [void](Shot 'workspace-double.png')
 Shortcut 90 # Undo highlighter
 if((Query 5) -ne 1) {throw 'Undo did not remove the multi-line highlight as one action'}
 $restored=Shot 'workspace-restored.png'
 if($restored -ne $marked) {throw 'Undo failed to restore exact selection'}
 Key 9 # Right pane must have no marks
 if((Query 5) -ne 0) {throw 'Highlights leaked into the other PDF'}
 Key 9;Key 80 # Left pen
 Click 280 320
 $originalHash=(Get-FileHash -LiteralPath $pdf).Hash
 [void](Send 0x800C)
 for($n=0;$n -lt 250 -and (Send 0x800D) -ne 0;$n++) {Start-Sleep -Milliseconds 20}
 if((Send 0x800D) -ne 0 -or !(Test-Path -LiteralPath ($pdf+'.exported.pdf'))) {throw 'Asynchronous PDF export failed'}
 if((Get-FileHash -LiteralPath $pdf).Hash -ne $originalHash) {throw 'Export modified original PDF'}
 Key 72
 Shortcut 49;Wait;Key 72
 if((Query 5) -ne 1) {throw 'Document highlight missing in individual workspace'}
 [void](Send 0x10);$p.WaitForExit();Launch
 Shortcut 49;Wait
 if((Query 5) -ne 1) {throw 'Text highlight not restored after restart'}
 Key 34;Wait;Key 72;Shortcut 90
 if((Query 5) -ne 0) {throw 'Undo text highlight failed after restart'}
 $glyphs=Glyphs
 HighlightText $glyphs[14] $glyphs[4]
 if((Query 5) -ne 1) {throw 'Reverse text selection failed'}
 Key 86;Shortcut 90
 if((Query 5) -ne 0) {throw 'Undo in reading mode did not remove last text selection'}
 Key 72
 [void](Send 0x201 1 (300 -bor (140 -shl 16)))
 [void](Send 0x200 1 (500 -bor (140 -shl 16)))
 [void](Send 0x202 0 (500 -bor (140 -shl 16)))
 if((Query 5) -ne 0) {throw 'Blank space created a freehand highlight'}
 # Click the second tab, close it, and keep the first PDF usable.
 Click 350 22;Wait
 if((Query 7) -ne 1) {throw 'Tab click failed'}
 Shortcut 87;Wait
 if((Query 0) -ne 1) {throw 'Closing a workspace failed'}
 Shortcut 87;Start-Sleep -Milliseconds 200
 if((Query 0) -ne 0) {throw 'Closing last workspace failed'}
 $file=[IO.File]::Open($pdf,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$file.Dispose()
 "PASS: tabs, combined workspace, independent navigation, workspace shortcuts, sidebar, text snapping, multiline/reverse selection, grouped undo, no freehand highlights, separate marks, annotation restart, tab click/close, releasing last PDF"
} finally {
 if($h -and $h -ne [IntPtr]::Zero) {[void](Send 0x10)}
 if($p -and !$p.HasExited) {[void]$p.WaitForExit(5000)}
}
