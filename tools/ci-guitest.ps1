# GUI-Starttest für GitHub Actions (Windows): startet QFiles, bedient es mit echter Maus und Tastatur
# und speichert Bildschirmfotos. Bei einem Absturz wird der Absturzbericht ausgegeben und der Test schlägt fehl.
param([string]$Exe = "build\Release\QFiles.exe")

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static void Click(int x, int y, bool right) {
    SetCursorPos(x, y); System.Threading.Thread.Sleep(150);
    uint down = right ? 0x0008u : 0x0002u, up = right ? 0x0010u : 0x0004u;
    mouse_event(down, 0, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(80);
    mouse_event(up, 0, 0, 0, IntPtr.Zero);
  }
}
"@

$crashTxt = "C:\ProgramData\QFiles\QFiles-Absturz.txt"
Remove-Item $crashTxt -ErrorAction SilentlyContinue
$env:QFILES_NO_CRASH_DIALOG = "1"

New-Item -ItemType Directory -Force -Path "C:\QFilesDemo\Ünïcödé 日本語 ✓" | Out-Null
Set-Content -Path "C:\QFilesDemo\Ünïcödé 日本語 ✓\Grüße €.txt" -Value "Hallo Welt – äöü 日本語" -Encoding UTF8
Set-Content -Path "C:\QFilesDemo\liesmich.txt" -Value "QFiles Starttest" -Encoding UTF8
Set-Content -Path "C:\QFilesDemo\zweite Datei.txt" -Value "Zweite Datei" -Encoding UTF8

function Shot([string]$name) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $bmp.Save("$PWD\$name")
  $g.Dispose(); $bmp.Dispose()
}

function Check-Alive([string]$step) {
  if ($script:p.HasExited) {
    $msg = "QFiles hat sich bei Schritt '$step' beendet (Exitcode $($script:p.ExitCode))."
    if (Test-Path $crashTxt) { $msg += "`n" + (Get-Content $crashTxt -Raw) }
    $msg | Tee-Object -FilePath smoke-log.txt
    exit 1
  }
}

$script:p = Start-Process -FilePath $Exe -ArgumentList '"C:\QFilesDemo"','"C:\Windows"' -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start"
Shot "screenshot.png"

# Linke obere Dateiliste suchen (sichtbares QFilesPane mit kleinster Position)
$main = $p.MainWindowHandle
[W]::SetForegroundWindow($main) | Out-Null
$best = [IntPtr]::Zero; $bestKey = [int]::MaxValue
$h = [IntPtr]::Zero
while ($true) {
  $h = [W]::FindWindowEx($main, $h, "QFilesPane", $null)
  if ($h -eq [IntPtr]::Zero) { break }
  if (-not [W]::IsWindowVisible($h)) { continue }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $key = $r.L + $r.T * 10
  if ($key -lt $bestKey) { $bestKey = $key; $best = $h }
}
$list = [W]::FindWindowEx($best, [IntPtr]::Zero, "SysListView32", $null)
$lr = New-Object W+RECT; [W]::GetWindowRect($list, [ref]$lr) | Out-Null
Write-Output "Liste: $($lr.L),$($lr.T)-$($lr.R),$($lr.B)"

# Zeile 3 (Index 2) = erste Datei "liesmich.txt" (nach ".." und dem Verzeichnis)
$x = $lr.L + 60
$y = $lr.T + 24 + 2 * 20 + 10
[W]::Click($x, $y, $false); Start-Sleep -Milliseconds 600
[W]::Click($x, $y, $true); Start-Sleep -Seconds 3
Check-Alive "Rechtsklick auf Datei"
Shot "screenshot3.png"
[System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800
Check-Alive "Kontextmenü schließen"

# Rechtsklick auf freie Fläche (Hintergrundmenü)
[W]::Click($x, $lr.B - 40, $true); Start-Sleep -Seconds 3
Check-Alive "Rechtsklick auf freie Fläche"
[System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800

# Mehrfachauswahl (Strg+A) und Rechtsklick
[W]::Click($x, $y, $false); Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("^a"); Start-Sleep -Milliseconds 400
[W]::Click($x, $y, $true); Start-Sleep -Seconds 3
Check-Alive "Rechtsklick bei Mehrfachauswahl"
Shot "screenshot4.png"
[System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800

# Rechtsklick auf ein Verzeichnis (Zeile 2)
$yd = $lr.T + 24 + 1 * 20 + 10
[W]::Click($x, $yd, $false); Start-Sleep -Milliseconds 400
[W]::Click($x, $yd, $true); Start-Sleep -Seconds 3
Check-Alive "Rechtsklick auf Verzeichnis"
[System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800

# Tastatur-Kontextmenü (Umschalt+F10) auf der Datei
[W]::Click($x, $y, $false); Start-Sleep -Milliseconds 500
[System.Windows.Forms.SendKeys]::SendWait("+{F10}"); Start-Sleep -Seconds 3
Check-Alive "Umschalt+F10"
[System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 800

# Rechte Liste teilen (Strg+T), links Datei wählen, Dateianzeige im anderen Fenster (F11)
[System.Windows.Forms.SendKeys]::SendWait("^2"); Start-Sleep -Milliseconds 500
[System.Windows.Forms.SendKeys]::SendWait("^t"); Start-Sleep -Milliseconds 800
[System.Windows.Forms.SendKeys]::SendWait("^1"); Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait("{HOME}{DOWN}{DOWN}"); Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait("{F11}"); Start-Sleep -Seconds 2
Check-Alive "Split und Dateianzeige"
Shot "screenshot2.png"

$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

# Abgefangener Fehler einer Kontextmenü-Erweiterung (simuliert): Meldung + Protokoll müssen erscheinen
$env:QFILES_TEST_SHELLFAULT = "1"
$log = "C:\ProgramData\QFiles\QFiles-Kontextmenue.txt"
Remove-Item $log -ErrorAction SilentlyContinue
$script:p = Start-Process -FilePath $Exe -ArgumentList '"C:\QFilesDemo"','"C:\Windows"' -PassThru
Start-Sleep -Seconds 6
Check-Alive "Start (Fehlersimulation)"
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
[W]::Click($x, $y, $false); Start-Sleep -Milliseconds 500
[W]::Click($x, $y, $true); Start-Sleep -Seconds 2
[System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Seconds 2
Shot "screenshot5.png"
Check-Alive "Fehlersimulation"
[System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Milliseconds 800
if (-not (Test-Path $log)) { "Protokoll $log wurde nicht geschrieben" | Tee-Object -FilePath smoke-log.txt; exit 1 }
Copy-Item $log .\kontextmenue-log.txt
Get-Content $log
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 2
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item Env:\QFILES_TEST_SHELLFAULT
if (-not (Test-Path "C:\ProgramData\QFiles\QFiles.ini")) {
  "Einstellungsdatei wurde nicht in C:\ProgramData\QFiles angelegt" | Tee-Object -FilePath smoke-log.txt; exit 1
}
Write-Output "GUI-Test bestanden; Einstellungsdatei: C:\ProgramData\QFiles\QFiles.ini"
exit 0
