# GUI-Starttest für GitHub Actions (Windows): startet QFiles, bedient es mit echter Maus und Tastatur
# und speichert Bildschirmfotos. Bei einem Absturz wird der Absturzbericht ausgegeben und der Test schlägt fehl.
param([string]$Exe = "build\Release\QFiles.exe", [string]$TestExt = "build\Release\QFilesTestExt.dll")

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
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
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

# Test-Kontextmenü-Erweiterung registrieren (HKLM: erhöhte Prozesse ignorieren HKCU-Klassen): A liegt in einem Pfad mit „ArchiCrypt“
# und muss durch den Standard-Ausschluss fehlen, B muss erscheinen.
$menuLog = "$PWD\menu-log.txt"
Remove-Item $menuLog -ErrorAction SilentlyContinue
$env:QFILES_TEST_MENULOG = $menuLog
$extA = "C:\QFilesTest\ArchiCrypt Testerweiterung\QFilesTestExt.dll"
$extB = "C:\QFilesTest\Normal\QFilesTestExt.dll"
foreach ($t in @($extA, $extB)) {
  New-Item -ItemType Directory -Force -Path (Split-Path $t) | Out-Null
  Copy-Item $TestExt $t -Force
}
$ext = @(@{ Id = "{6E3A0C41-8F0B-4C55-9C1D-51A1E5F0A001}"; Dll = $extA; Name = "QFilesTestA" },
         @{ Id = "{6E3A0C41-8F0B-4C55-9C1D-51A1E5F0A002}"; Dll = $extB; Name = "QFilesTestB" })
foreach ($e in $ext) {
  & reg.exe add "HKLM\Software\Classes\CLSID\$($e.Id)" /ve /d "QFiles Testerweiterung $($e.Name)" /f | Out-Null
  & reg.exe add "HKLM\Software\Classes\CLSID\$($e.Id)\InprocServer32" /ve /d "$($e.Dll)" /f | Out-Null
  & reg.exe add "HKLM\Software\Classes\CLSID\$($e.Id)\InprocServer32" /v ThreadingModel /d Apartment /f | Out-Null
  foreach ($k in @("*", "Directory", "Directory\Background")) {
    & reg.exe add "HKLM\Software\Classes\$k\shellex\ContextMenuHandlers\$($e.Name)" /ve /d "$($e.Id)" /f | Out-Null
  }
}

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

# Optionen: „Dateidatum sekundengenau“ (Alt+K) und „Spalte Erstellt“ (Alt+E) per Zugriffstaste einschalten,
# mit OK speichern (prüft auch, dass die Zugriffstasten eindeutig sind und Kontrollkästchen umschalten)
[W]::SetForegroundWindow($main) | Out-Null
[System.Windows.Forms.SendKeys]::SendWait("%w"); Start-Sleep -Milliseconds 600
[System.Windows.Forms.SendKeys]::SendWait("o"); Start-Sleep -Seconds 2
$dlg = [W]::FindWindow("#32770", "Optionen")
if ($dlg -eq [IntPtr]::Zero) { "Optionen-Dialog nicht gefunden" | Tee-Object -FilePath smoke-log.txt; exit 1 }
[W]::SetForegroundWindow($dlg) | Out-Null
[System.Windows.Forms.SendKeys]::SendWait("%k"); Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("%e"); Start-Sleep -Milliseconds 400
Shot "screenshot6.png"
[System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Seconds 2
Check-Alive "Optionen"
Shot "screenshot7.png"
$ini = Get-Content "C:\ProgramData\QFiles\QFiles.ini" -Raw -Encoding UTF8
if ($ini -notmatch "(?m)^DatumSekunden=1\s*$" -or $ini -notmatch "(?m)^SpalteErstellt=1\s*$") {
  $opt = ($ini -split "`r?`n" | Where-Object { $_ -match "DatumSekunden|SpalteErstellt" }) -join ' | '
  "Optionen sekundengenau/Spalte Erstellt wurden nicht gespeichert: $opt" | Tee-Object -FilePath smoke-log.txt
  exit 1
}
Write-Output "::notice title=Optionen::DatumSekunden=1 und SpalteErstellt=1 gespeichert."

$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

# Ausschluss von Kontextmenü-Erweiterungen prüfen
Remove-Item Env:\QFILES_TEST_MENULOG
if (-not (Test-Path $menuLog)) { "Menüprotokoll $menuLog wurde nicht geschrieben" | Tee-Object -FilePath smoke-log.txt; exit 1 }
$menuText = Get-Content $menuLog -Raw
Get-Content $menuLog | Select-Object -First 60
if ($menuText -match "QFiles-Testeintrag A") {
  "Ausgeschlossene Test-Erweiterung (Pfad mit ArchiCrypt) erscheint trotzdem im Kontextmenü" | Tee-Object -FilePath smoke-log.txt
  exit 1
}
if ($menuText -match "QFiles-Testeintrag B") {
  Write-Output "::notice title=Kontextmenü-Ausschluss::Test-Erweiterung B geladen, A (ArchiCrypt) ausgeschlossen."
} else {
  $short = (($menuText -split "`r?`n") | Select-Object -First 30) -join ' | '
  Write-Output "::warning title=Kontextmenü-Ausschluss::Test-Erweiterung B wurde nicht geladen – Ausschluss nicht aussagekräftig geprüft. Menü: $short"
}
foreach ($e in $ext) {
  & reg.exe delete "HKLM\Software\Classes\CLSID\$($e.Id)" /f | Out-Null
  foreach ($k in @("*", "Directory", "Directory\Background")) {
    & reg.exe delete "HKLM\Software\Classes\$k\shellex\ContextMenuHandlers\$($e.Name)" /f | Out-Null
  }
}

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

# ---- Netzwerk und FTP ----
# Freigabe auf diesem Rechner; Liste 1 zeigt \\localhost (Freigaben), Liste 2 einen anonymen FTP-Zugang.
$paneLog = "$PWD\pane-log.txt"
Remove-Item $paneLog -ErrorAction SilentlyContinue
$env:QFILES_TEST_PANELOG = $paneLog
try { New-SmbShare -Name "QFilesTest" -Path "C:\QFilesDemo" -ReadAccess "Everyone" -ErrorAction Stop | Out-Null } catch { Write-Output "Freigabe: $_" }
New-Item -ItemType Directory -Force -Path "C:\QFilesFtp\Ordner Ä" | Out-Null
Set-Content -Path "C:\QFilesFtp\Grüße vom FTP.txt" -Value "Hallo FTP – äöü" -Encoding UTF8
$ftpd = Start-Process python -ArgumentList 'tools\ci-ftpd.py','C:\QFilesFtp','2121' -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3
$script:p = Start-Process -FilePath $Exe -ArgumentList '"\\localhost"','"ftp://127.0.0.1:2121/"' -PassThru
Start-Sleep -Seconds 10
Check-Alive "Start (Netzwerk/FTP)"
Shot "screenshot8.png"
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
# Liste 1: in die Freigabe wechseln
[System.Windows.Forms.SendKeys]::SendWait("^1"); Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("^l"); Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("\\localhost\QFilesTest{ENTER}"); Start-Sleep -Seconds 3
Check-Alive "UNC-Freigabe"
# Liste 1: Netzwerk durchsuchen (Esc bricht ab)
[System.Windows.Forms.SendKeys]::SendWait("^l"); Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("Netzwerk{ENTER}"); Start-Sleep -Seconds 8
Shot "screenshot9.png"
[System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Seconds 1
Check-Alive "Netzwerk"
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Stop-Process -Id $ftpd.Id -Force -ErrorAction SilentlyContinue
Remove-Item Env:\QFILES_TEST_PANELOG
if (-not (Test-Path $paneLog)) { "Listenprotokoll fehlt" | Tee-Object -FilePath smoke-log.txt; exit 1 }
$pl = Get-Content $paneLog -Encoding UTF8
$pl | ForEach-Object { Write-Output $_ }
$okShare = $pl | Where-Object { $_ -match '^Liste 1 \| \\\\localhost \|.*QFilesTest' }
$okFtp = $pl | Where-Object { $_ -match '^Liste 2 \| ftp://127\.0\.0\.1:2121/ \|.*Grüße vom FTP\.txt' }
$okUnc = $pl | Where-Object { $_ -match '^Liste 1 \| \\\\localhost\\QFilesTest \|.*liesmich\.txt' }
if (-not $okFtp) { "FTP-Liste nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1 }
if (-not $okShare) { Write-Output "::warning title=Netzwerk::Freigabe QFilesTest von \\localhost nicht gefunden (siehe pane-log.txt)" }
if (-not $okUnc) { Write-Output "::warning title=Netzwerk::Inhalt von \\localhost\QFilesTest nicht gelesen (siehe pane-log.txt)" }
if ($okFtp -and $okShare -and $okUnc) { Write-Output "::notice title=Netzwerk/FTP::Freigaben von \\localhost, UNC-Verzeichnis und FTP-Liste gelesen." }
$netLine = $pl | Where-Object { $_ -match '^Liste 1 \| \\\\ \|' } | Select-Object -Last 1
if ($netLine) { Write-Output "::notice title=Netzwerk-Suche::$netLine" }

if (-not (Test-Path "C:\ProgramData\QFiles\QFiles.ini")) {
  "Einstellungsdatei wurde nicht in C:\ProgramData\QFiles angelegt" | Tee-Object -FilePath smoke-log.txt; exit 1
}
Write-Output "GUI-Test bestanden; Einstellungsdatei: C:\ProgramData\QFiles\QFiles.ini"
exit 0
