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
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  public static void Drag(int x1, int y1, int x2, int y2) {
    SetCursorPos(x1, y1); System.Threading.Thread.Sleep(200);
    mouse_event(0x0002, 0, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(150);
    for (int i = 1; i <= 25; i++) {
      SetCursorPos(x1 + (x2 - x1) * i / 25, y1 + (y2 - y1) * i / 25);
      mouse_event(0x0001, 0, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(40);
    }
    SetCursorPos(x2, y2); mouse_event(0x0001, 0, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(400);
    mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
  }
  public static void Key(byte vk) { keybd_event(vk, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(60); keybd_event(vk, 0, 2, IntPtr.Zero); }
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  public static IntPtr FindDescendant(IntPtr parent, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumChildWindows(parent, (h, l) => {
      var sb = new System.Text.StringBuilder(64); GetClassName(h, sb, 64);
      if (sb.ToString() == cls && IsWindowVisible(h)) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
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
# „Optionen“ steht direkt in der Menüleiste (Alt+O)
[System.Windows.Forms.SendKeys]::SendWait("%o"); Start-Sleep -Seconds 2
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
Start-Sleep -Seconds 8
Check-Alive "Start (Fehlersimulation)"
# Rechtsklick auf die Datei öffnet das Kontextmenü (mit der simulierten Ausnahme). Der Rechner ist mitunter noch mit dem
# Start beschäftigt – bis zu drei Versuche, bis das Protokoll geschrieben ist.
for ($try = 1; $try -le 3 -and -not (Test-Path $log); $try++) {
  [W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
  Start-Sleep -Milliseconds 500
  [W]::Click($x, $y, $false); Start-Sleep -Milliseconds 700
  [W]::Click($x, $y, $true); Start-Sleep -Seconds 3
  [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Seconds 2
  if ($try -eq 1) { Shot "screenshot5.png" }
  Check-Alive "Fehlersimulation"
  [System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Seconds 1
  if (-not (Test-Path $log)) { Write-Output "Fehlersimulation: Versuch $try ohne Protokoll" }
}
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
# Liste 1: Ansicht "Mit Unterverzeichnissen" (Strg+Umschalt+5) im Demo-Verzeichnis
[System.Windows.Forms.SendKeys]::SendWait("^l"); Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("C:\QFilesDemo{ENTER}"); Start-Sleep -Seconds 2
[System.Windows.Forms.SendKeys]::SendWait("^+5"); Start-Sleep -Seconds 3
Shot "screenshot10.png"
Check-Alive "Ansicht mit Unterverzeichnissen"
[System.Windows.Forms.SendKeys]::SendWait("^+1"); Start-Sleep -Milliseconds 800
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
$recLine = $pl | Where-Object { $_ -match '^Liste 1 \| C:\\QFilesDemo \(rekursiv\) \|' } | Select-Object -Last 1
if (-not $recLine -or $recLine -notmatch 'Grüße €\.txt') { "Ansicht mit Unterverzeichnissen nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1 }
Write-Output "::notice title=Mit Unterverzeichnissen::$recLine"
$netLine = $pl | Where-Object { $_ -match '^Liste 1 \| \\\\ \|' } | Select-Object -Last 1
if ($netLine) { Write-Output "::notice title=Netzwerk-Suche::$netLine" }

# ---- PDF- und Video-Anzeige (F11): Testdateien erzeugen, nacheinander in der Dateianzeige zeigen ----
$mediaDir = "C:\QFilesMedien"
& python tools\make-test-media.py $mediaDir
$viewLog = "$PWD\viewer-log.txt"
Remove-Item $viewLog -ErrorAction SilentlyContinue
$env:QFILES_TEST_VIEWERLOG = $viewLog
$script:p = Start-Process -FilePath $Exe -ArgumentList ('"' + $mediaDir + '"'),'"C:\Windows"' -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start (Medien)"
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
# Liste 1 suchen und die erste Datei anklicken (Zeile 2 nach „..“), damit die Tasten ankommen
$best = [IntPtr]::Zero; $bestKey = [int]::MaxValue; $h = [IntPtr]::Zero
while ($true) {
  $h = [W]::FindWindowEx($p.MainWindowHandle, $h, "QFilesPane", $null)
  if ($h -eq [IntPtr]::Zero) { break }
  if (-not [W]::IsWindowVisible($h)) { continue }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $key = $r.L + $r.T * 10
  if ($key -lt $bestKey) { $bestKey = $key; $best = $h }
}
$mlist = [W]::FindWindowEx($best, [IntPtr]::Zero, "SysListView32", $null)
$mr = New-Object W+RECT; [W]::GetWindowRect($mlist, [ref]$mr) | Out-Null
[W]::Click($mr.L + 60, $mr.T + 24 + 1 * 20 + 10, $false); Start-Sleep -Milliseconds 800
[System.Windows.Forms.SendKeys]::SendWait("{HOME}{DOWN}"); Start-Sleep -Milliseconds 400
# Wartet, bis die Dateianzeige ein weiteres Ergebnis protokolliert hat (höchstens 40 s; Media Foundation braucht
# beim ersten Video auf dem CI-Rechner mitunter lange)
function Wait-ViewerLog([int]$count) {
  for ($t = 0; $t -lt 80; $t++) {
    if ((Test-Path $viewLog) -and @(Get-Content $viewLog -Encoding UTF8).Count -ge $count) { break }
    Start-Sleep -Milliseconds 500
  }
  Start-Sleep -Milliseconds 800
}
[System.Windows.Forms.SendKeys]::SendWait("{F11}")
Wait-ViewerLog 1
Shot "screenshot11.png"   # 1 Seite
$shots = @{ 1 = "screenshot12.png"; 2 = "screenshot13.png"; 3 = "screenshot14.png"; 5 = "screenshot15.png"; 6 = "screenshot16.png" }
for ($i = 1; $i -le 7; $i++) {
  [System.Windows.Forms.SendKeys]::SendWait("{DOWN}")
  Wait-ViewerLog ($i + 1)
  Check-Alive "Dateianzeige Medien $i"
  if ($shots.ContainsKey($i)) { Shot $shots[$i] }
}
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item Env:\QFILES_TEST_VIEWERLOG
if (-not (Test-Path $viewLog)) { "Protokoll der Dateianzeige fehlt" | Tee-Object -FilePath smoke-log.txt; exit 1 }
$vl = Get-Content $viewLog -Encoding UTF8
$vl | ForEach-Object { Write-Output $_ }
$pdfOk = @($vl | Where-Object { $_ -match '\.pdf \| PDF' }).Count
$pdfFail = @($vl | Where-Object { $_ -match '\.pdf \|' -and $_ -notmatch '\.pdf \| PDF' })
if ($pdfOk -lt 5) {
  Write-Output "::error title=PDF-Anzeige::Nur $pdfOk von 5 PDFs angezeigt: $($pdfFail -join ' / ')"
  "PDF-Anzeige fehlgeschlagen" | Tee-Object -FilePath smoke-log.txt; exit 1
}
Write-Output ("::notice title=PDF-Anzeige::" + (($vl | Where-Object { $_ -match '\.pdf \|' }) -join ' / '))
$vids = @($vl | Where-Object { $_ -match '\.avi \|' })
$vidOk = @($vids | Where-Object { $_ -match 'Auflösung' -and $_ -notmatch 'nicht möglich' }).Count
if ($vidOk -eq 3) { Write-Output ("::notice title=Video-Anzeige::" + ($vids -join ' / ')) }
else { Write-Output ("::warning title=Video-Anzeige::" + ($vids -join ' / ')) }

# ---- Archive in der Dateianzeige (F11): ZIP, TAR/GZ (über tar.exe), kennwortgeschützt, beschädigt ----
# Dazu „Listenanzeige gestreift“ und „Nicht aktive Listen grau“ einschalten (spätere Werte gelten)
$ini = "C:\ProgramData\QFiles\QFiles.ini"
if (Test-Path $ini) { Add-Content -Path $ini -Value "`r`n[Optionen]`r`nGestreift=1`r`nInaktiveListenGrau=1" -Encoding UTF8 }
$arcDir = "C:\QFilesArchive"
& python tools\make-test-archives.py $arcDir
# Deutsche Sprache: tar.exe schreibt dann „Mai“, „Okt“ … statt „May“, „Oct“ (setlocale), das muss QFiles verstehen
$oldCulture = (Get-Culture).Name
Set-Culture de-DE
$tarExe = "$env:SystemRoot\System32\tar.exe"
if (Test-Path $tarExe) {
  $tarOut = (& $tarExe -tvf "$arcDir\e-Projekt.rar" 2>&1 | Out-String).Trim() -replace "`r?`n", ' / '
  Write-Output "::notice title=tar.exe (de-DE)::$tarOut"
}
# subst-Laufwerk für die Laufwerkssymbole in der Kopfzeile (Bildschirmfoto 17)
& subst.exe S: $arcDir
$arcZip = (Get-Item "$arcDir\a-Projekt.zip").LastWriteTimeUtc
$arcHash = (Get-FileHash "$arcDir\a-Projekt.zip").Hash
Remove-Item $viewLog -ErrorAction SilentlyContinue
$env:QFILES_TEST_VIEWERLOG = $viewLog
$script:p = Start-Process -FilePath $Exe -ArgumentList ('"' + $arcDir + '"'),'"C:\Windows"' -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start (Archive)"
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
$best = [IntPtr]::Zero; $bestKey = [int]::MaxValue; $h = [IntPtr]::Zero
while ($true) {
  $h = [W]::FindWindowEx($p.MainWindowHandle, $h, "QFilesPane", $null)
  if ($h -eq [IntPtr]::Zero) { break }
  if (-not [W]::IsWindowVisible($h)) { continue }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $key = $r.L + $r.T * 10
  if ($key -lt $bestKey) { $bestKey = $key; $best = $h }
}
$alist = [W]::FindWindowEx($best, [IntPtr]::Zero, "SysListView32", $null)
$ar = New-Object W+RECT; [W]::GetWindowRect($alist, [ref]$ar) | Out-Null
[W]::Click($ar.L + 60, $ar.T + 24 + 1 * 20 + 10, $false); Start-Sleep -Milliseconds 800
[System.Windows.Forms.SendKeys]::SendWait("{HOME}{DOWN}"); Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("{F11}")
Wait-ViewerLog 1
Shot "screenshot17.png"   # ZIP als Liste (Liste 1 aktiv: Archivliste abgetönt gelb)
# Kontextmenü „Anzeigen“ auf liesmich.txt (letzte Zeile: Alt, Bilder, Quellen, Grüße €.txt, liesmich.txt): eigenes
# Anzeigefenster, das offen bleibt – auch wenn danach ins QFiles-Fenster geklickt wird
$arcView = [W]::FindDescendant($p.MainWindowHandle, "QFilesArchiveView")
$arcList = if ($arcView -ne [IntPtr]::Zero) { [W]::FindWindowEx($arcView, [IntPtr]::Zero, "SysListView32", $null) } else { [IntPtr]::Zero }
if ($arcList -eq [IntPtr]::Zero) { "Archivliste nicht gefunden" | Tee-Object -FilePath smoke-log.txt; exit 1 }
$lr = New-Object W+RECT; [W]::GetWindowRect($arcList, [ref]$lr) | Out-Null
# (die Liste ist auf dem kleinen CI-Bildschirm niedrig: erste Zeile anklicken, mit Ende zu liesmich.txt, Kontextmenütaste)
[W]::Click($lr.L + 60, $lr.T + 24 + 10, $false); Start-Sleep -Milliseconds 600
[System.Windows.Forms.SendKeys]::SendWait("{END}"); Start-Sleep -Milliseconds 600
[W]::Key(0x5D); Start-Sleep -Milliseconds 800
[System.Windows.Forms.SendKeys]::SendWait("a"); Start-Sleep -Seconds 3
$vf = [W]::FindWindow("QFilesViewerFrame", [NullString]::Value)   # $null würde zu ""
$vfOk = ($vf -ne [IntPtr]::Zero) -and [W]::IsWindowVisible($vf)
$fg = [W]::GetForegroundWindow()
$vfFront = $vfOk -and ($fg -eq $vf)
$fgCls = New-Object System.Text.StringBuilder 64; [W]::GetClassName($fg, $fgCls, 64) | Out-Null
$fgInfo = "Vordergrund: " + $fgCls.ToString() + $(if ($fg -eq $p.MainWindowHandle) { " (QFiles-Hauptfenster)" } else { "" })
Shot "screenshot21.png"
# in die Verzeichnisliste klicken: das Anzeigefenster muss sichtbar bleiben (gehört zum QFiles-Fenster)
[W]::Click($ar.L + 200, $ar.B - 40, $false); Start-Sleep -Seconds 2
$vfStill = ($vf -ne [IntPtr]::Zero) -and [W]::IsWindowVisible($vf) -and ([W]::GetWindow($vf, 4) -eq $p.MainWindowHandle)
if (-not ($vfOk -and $vfFront -and $vfStill)) {
  Write-Output "::error title=Anzeigen aus Archiv::Fenster sichtbar: $vfOk, im Vordergrund: $vfFront ($fgInfo), nach Klick noch sichtbar: $vfStill"
  "Anzeigefenster aus der Archivliste nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}
Write-Output "::notice title=Anzeigen aus Archiv::Fenster sichtbar, im Vordergrund: $vfFront, bleibt nach Klick ins Hauptfenster offen"
[W]::SendMessage($vf, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
Start-Sleep -Milliseconds 500
$arcShots = @{ 1 = "screenshot18.png"; 2 = "screenshot19.png"; 4 = "screenshot20.png" }
for ($i = 1; $i -le 4; $i++) {
  [System.Windows.Forms.SendKeys]::SendWait("{DOWN}")
  Wait-ViewerLog ($i + 1)
  Check-Alive "Dateianzeige Archiv $i"
  if ($arcShots.ContainsKey($i)) { Shot $arcShots[$i] }
}
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item Env:\QFILES_TEST_VIEWERLOG
Set-Culture $oldCulture
& subst.exe S: /D
$al = @()
if (Test-Path $viewLog) { $al = Get-Content $viewLog -Encoding UTF8 }
$al | ForEach-Object { Write-Output $_ }
$zipOk = $al | Where-Object { $_ -match '^a-Projekt\.zip \| Archiv \| \d+ Verz\. \| 6 Dateien \| Datum bei (\d+) von \1$' }
$tarOk = $al | Where-Object { $_ -match '^b-Quellen\.tar\.gz \| Archiv \|.*6 Dateien \| Datum bei (\d+) von \1$' }
$rarOk = $al | Where-Object { $_ -match '^e-Projekt\.rar \| Archiv \| 4 Verz\. \| 6 Dateien \| Datum bei 10 von 10$' }
$pwOk = $al | Where-Object { $_ -match '^c-geheim\.zip \| Archivfehler \|.*kennwortgeschützt' }
$badOk = $al | Where-Object { $_ -match '^d-kaputt\.zip \| Archivfehler \|' }
if (-not ($zipOk -and $pwOk -and $badOk)) {
  Write-Output ("::error title=Archivanzeige::" + ($al -join ' / '))
  "Archivanzeige nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}
if ((Get-FileHash "$arcDir\a-Projekt.zip").Hash -ne $arcHash -or (Get-Item "$arcDir\a-Projekt.zip").LastWriteTimeUtc -ne $arcZip) {
  "Archiv wurde durch die Anzeige verändert" | Tee-Object -FilePath smoke-log.txt; exit 1
}
# Manche Windows-Abbilder: tar.exe überspringt Namen außerhalb der Systemcodepage („tief ✓.txt“) – dann fehlt
# genau diese Datei in TAR/GZ und RAR; das ist eine Grenze von tar.exe, kein Fehler von QFiles (Warnung)
$tarCp = $al | Where-Object { $_ -match '^b-Quellen\.tar\.gz \| Archiv \|.*5 Dateien \| Datum bei (\d+) von \1$' }
$rarCp = $al | Where-Object { $_ -match '^e-Projekt\.rar \| Archiv \| 4 Verz\. \| 5 Dateien \| Datum bei 9 von 9$' }
if ((-not $tarOk -and $tarCp) -or (-not $rarOk -and $rarCp)) {
  Write-Output ("::warning title=Archivanzeige (tar.exe, Codepage)::" + ($al -join ' / '))
  if ($tarCp) { $tarOk = $true }
  if ($rarCp) { $rarOk = $true }
}
if (-not $tarOk -or -not $rarOk) {
  # tar.exe ist vorhanden (Windows Server 2025); TAR/GZ und RAR müssen auch mit deutschen Monatsnamen gelesen werden
  Write-Output ("::error title=Archivanzeige (tar.exe)::" + ($al -join ' / '))
  "Archivanzeige über tar.exe nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}
if ($tarOk) { Write-Output ("::notice title=Archivanzeige::" + ($al -join ' / ')) }
else { Write-Output ("::warning title=Archivanzeige (tar.exe)::" + ($al -join ' / ')) }

# ---- Ziehen in eine Liste: Ziel je nach Spalte und Stelle in der Namensspalte ----
# Links Quelle mit q1–q3, rechts Ziel mit dem Unterverzeichnis „Unterordner“ (Details-Ansicht).
# q1 auf den Namen „Unterordner“ -> ins Unterverzeichnis; q2 in den Leerraum hinter dem Namen -> Abfrage, „Listenverzeichnis“;
# q3 in die Spalte „Typ“ der Verzeichniszeile -> ins Listenverzeichnis (ohne Abfrage).
$dragBase = "C:\QFilesZiehen"
Remove-Item $dragBase -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$dragBase\Quelle", "$dragBase\Ziel\Unterordner" | Out-Null
foreach ($n in "q1", "q2", "q3") { Set-Content -Path "$dragBase\Quelle\$n.txt" -Value $n -Encoding UTF8 }
$script:p = Start-Process -FilePath $Exe -ArgumentList ('"' + "$dragBase\Quelle" + '"'), ('"' + "$dragBase\Ziel" + '"') -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start (Ziehen)"
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
$panes = @()
$h = [IntPtr]::Zero
while ($true) {
  $h = [W]::FindWindowEx($p.MainWindowHandle, $h, "QFilesPane", $null)
  if ($h -eq [IntPtr]::Zero) { break }
  if (-not [W]::IsWindowVisible($h)) { continue }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $panes += [pscustomobject]@{ H = $h; L = $r.L; T = $r.T }
}
$panes = @($panes | Sort-Object T, L)
$srcList = [W]::FindWindowEx($panes[0].H, [IntPtr]::Zero, "SysListView32", $null)
$dstList = [W]::FindWindowEx($panes[1].H, [IntPtr]::Zero, "SysListView32", $null)
$sr = New-Object W+RECT; [W]::GetWindowRect($srcList, [ref]$sr) | Out-Null
$dr = New-Object W+RECT; [W]::GetWindowRect($dstList, [ref]$dr) | Out-Null
# Zielliste: Details-Ansicht
[W]::Click($dr.L + 60, $dr.B - 30, $false); Start-Sleep -Milliseconds 500
[System.Windows.Forms.SendKeys]::SendWait("^+1"); Start-Sleep -Milliseconds 800
[W]::GetWindowRect($dstList, [ref]$dr) | Out-Null
# Namensspalte auf 160 px setzen (LVM_SETCOLUMNWIDTH), damit auch die Spalte „Typ“ in der schmalen Liste sichtbar ist
[W]::SendMessage($dstList, 0x101E, [IntPtr]::Zero, [IntPtr]160) | Out-Null
Start-Sleep -Milliseconds 300
$nameW = ([W]::SendMessage($dstList, 0x101D, [IntPtr]::Zero, [IntPtr]::Zero)).ToInt32()   # LVM_GETCOLUMNWIDTH
$rowY = $dr.T + 24 + 1 * 20 + 10     # Zeile 2 (nach „..“): Unterordner
$srcY = $sr.T + 24 + 1 * 20 + 10     # Zeile 2 der Quelle: jeweils die erste verbliebene Datei
Write-Output "Ziehen: Namensspalte $nameW px"
# 1) auf den Namen
[W]::Click($sr.L + 40, $srcY, $false); Start-Sleep -Milliseconds 400
[W]::Drag($sr.L + 40, $srcY, $dr.L + 45, $rowY); Start-Sleep -Seconds 3
Check-Alive "Ziehen 1"
# 2) Leerraum hinter dem Namen: Abfrage „Listenverzeichnis“
[W]::Click($sr.L + 40, $srcY, $false); Start-Sleep -Milliseconds 400
[W]::Drag($sr.L + 40, $srcY, $dr.L + $nameW - 12, $rowY); Start-Sleep -Seconds 2
$ask = [IntPtr]::Zero
for ($t = 0; $t -lt 20 -and $ask -eq [IntPtr]::Zero; $t++) {
  $ask = [W]::FindWindow("#32770", "Verschieben")
  if ($ask -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 }
}
$askSeen = $ask -ne [IntPtr]::Zero
if ($askSeen) {
  Shot "screenshot22.png"
  [W]::SendMessage([W]::GetDlgItem($ask, 201), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # BM_CLICK „Listenverzeichnis“
  Start-Sleep -Seconds 3
}
Check-Alive "Ziehen 2"
# 3) Spalte „Typ“ der Verzeichniszeile
[W]::Click($sr.L + 40, $srcY, $false); Start-Sleep -Milliseconds 400
[W]::Drag($sr.L + 40, $srcY, $dr.L + $nameW + 25, $rowY); Start-Sleep -Seconds 3
Check-Alive "Ziehen 3"
Shot "screenshot23.png"
# 4) innerhalb der Zielliste: q3 (liegt schon im Listenverzeichnis) in den Leerraum hinter „Unterordner“ -> ebenfalls
#    Abfrage; „Abbrechen“ lässt q3 an seinem Platz (Zeilen: .., Unterordner, q2.txt, q3.txt)
$q3Y = $dr.T + 24 + 3 * 20 + 10
[W]::Click($dr.L + 40, $q3Y, $false); Start-Sleep -Milliseconds 400
[W]::Drag($dr.L + 40, $q3Y, $dr.L + $nameW - 12, $rowY); Start-Sleep -Seconds 2
$ask2 = [IntPtr]::Zero
for ($t = 0; $t -lt 20 -and $ask2 -eq [IntPtr]::Zero; $t++) {
  $ask2 = [W]::FindWindow("#32770", "Verschieben")
  if ($ask2 -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 }
}
$askSame = $ask2 -ne [IntPtr]::Zero
if ($askSame) {
  [W]::SendMessage([W]::GetDlgItem($ask2, 2), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # BM_CLICK „Abbrechen“
  Start-Sleep -Seconds 2
}
Check-Alive "Ziehen 4"
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
$d1 = Test-Path "$dragBase\Ziel\Unterordner\q1.txt"
$d2 = (Test-Path "$dragBase\Ziel\q2.txt") -and $askSeen
$d3 = (Test-Path "$dragBase\Ziel\q3.txt") -and -not (Test-Path "$dragBase\Ziel\Unterordner\q3.txt")
$d4 = $askSame -and (Test-Path "$dragBase\Ziel\q3.txt")
$dragInfo = "Name -> Unterverzeichnis: $d1; Leerraum -> Abfrage/Listenverzeichnis: $d2; Spalte Typ -> Listenverzeichnis: $d3; gleiche Liste -> Abfrage: $d4"
$left = (Get-ChildItem "$dragBase" -Recurse -File | ForEach-Object { $_.FullName.Substring($dragBase.Length) }) -join ', '
if ($d1 -and $d2 -and $d3 -and $d4) { Write-Output "::notice title=Ziehen in Liste::$dragInfo" }
else {
  Write-Output "::error title=Ziehen in Liste::$dragInfo (Dateien: $left)"
  "Ablageziel beim Ziehen nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}

# ---- Markieren durch Ziehen (Details, wie Idoswin Pro), Ziehen aus der Liste heraus, mittlere Maustaste ----
# Zeilen links: .., Unter, m01 … m12, „m13 mit einem deutlich längeren Namen“. 1) Auswahlrahmen rechts neben den Namen
# über m03 bis m07 bis in die Namen hinein markiert 5 Dateien. 2) Schmaler Rahmen rechts der kurzen Namen über alle
# Zeilen erfasst nur den langen Namen. 3) Ziehen auf dem Namen m01 (nicht markiert) waagrecht in die rechte Liste:
# Drag & Drop wie bisher verschiebt m01. 4) „Mit Unterverzeichnissen“, mittlere Maustaste auf Unter\x.txt: Liste
# wechselt nach „Unter“ und markiert x.txt.
$mBase = "C:\QFilesMarkieren"
Remove-Item $mBase -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$mBase\Liste\Unter", "$mBase\Ziel" | Out-Null
foreach ($i in 1..12) { Set-Content -Path ("$mBase\Liste\m{0:D2}.txt" -f $i) -Value $i -Encoding UTF8 }
Set-Content -Path "$mBase\Liste\m13 mit einem deutlich längeren Namen.txt" -Value "13" -Encoding UTF8
Set-Content -Path "$mBase\Liste\Unter\x.txt" -Value "x" -Encoding UTF8
$script:p = Start-Process -FilePath $Exe -ArgumentList ('"' + "$mBase\Liste" + '"'), ('"' + "$mBase\Ziel" + '"') -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start (Markieren durch Ziehen)"
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
$panes = @()
$h = [IntPtr]::Zero
while ($true) {
  $h = [W]::FindWindowEx($p.MainWindowHandle, $h, "QFilesPane", $null)
  if ($h -eq [IntPtr]::Zero) { break }
  if (-not [W]::IsWindowVisible($h)) { continue }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $panes += [pscustomobject]@{ H = $h; L = $r.L; T = $r.T }
}
$panes = @($panes | Sort-Object T, L)
$mList = [W]::FindWindowEx($panes[0].H, [IntPtr]::Zero, "SysListView32", $null)
$zList = [W]::FindWindowEx($panes[1].H, [IntPtr]::Zero, "SysListView32", $null)
$mr = New-Object W+RECT; [W]::GetWindowRect($mList, [ref]$mr) | Out-Null
$zr = New-Object W+RECT; [W]::GetWindowRect($zList, [ref]$zr) | Out-Null
[W]::Click($mr.L + 60, $mr.B - 30, $false); Start-Sleep -Milliseconds 500
[System.Windows.Forms.SendKeys]::SendWait("^+1"); Start-Sleep -Milliseconds 800
[W]::GetWindowRect($mList, [ref]$mr) | Out-Null
[W]::SendMessage($mList, 0x101E, [IntPtr]::Zero, [IntPtr]300) | Out-Null   # Namensspalte 300 px
Start-Sleep -Milliseconds 300
# Zeilenhöhe aus LVM_APPROXIMATEVIEWRECT (Höhe für 1 bzw. 11 Elemente), erste Zeile unter der Spaltenüberschrift
$hv1 = ([W]::SendMessage($mList, 0x1040, [IntPtr]1, [IntPtr]-1)).ToInt64()
$hv11 = ([W]::SendMessage($mList, 0x1040, [IntPtr]11, [IntPtr]-1)).ToInt64()
$rowH = [int]((($hv11 -shr 16) -band 0xFFFF) - (($hv1 -shr 16) -band 0xFFFF)) / 10
$mHdr = [W]::FindWindowEx($mList, [IntPtr]::Zero, "SysHeader32", $null)
$hr = New-Object W+RECT; [W]::GetWindowRect($mHdr, [ref]$hr) | Out-Null
if ($rowH -lt 10 -or $rowH -gt 60) { $rowH = 20 }
Write-Output "Markieren durch Ziehen: Zeilenhöhe $rowH, Überschrift bis $($hr.B)"
function RowY([int]$row) { return [int]($hr.B + $row * $rowH + $rowH / 2) }
[W]::Drag($mr.L + 200, (RowY 4), $mr.L + 40, (RowY 8)); Start-Sleep -Milliseconds 800
$selCount1 = ([W]::SendMessage($mList, 0x1032, [IntPtr]::Zero, [IntPtr]::Zero)).ToInt32()   # LVM_GETSELECTEDCOUNT
Shot "screenshot34.png"   # m03–m07 durch Ziehen markiert
[W]::Drag($mr.L + 160, (RowY 2), $mr.L + 175, (RowY 14)); Start-Sleep -Milliseconds 800
$selCount2 = ([W]::SendMessage($mList, 0x1032, [IntPtr]::Zero, [IntPtr]::Zero)).ToInt32()
Shot "screenshot36.png"   # schmaler Rahmen: nur der lange Name
# m01 (Zeile 2) waagrecht in die leere obere rechte Liste ziehen
[W]::Drag($mr.L + 40, (RowY 2), $zr.L + 120, (RowY 2)); Start-Sleep -Seconds 3
Check-Alive "Ziehen aus der Liste heraus"
$moved = (Test-Path "$mBase\Ziel\m01.txt") -and -not (Test-Path "$mBase\Liste\m01.txt")
[W]::Click($mr.L + 60, $mr.B - 30, $false); Start-Sleep -Milliseconds 500
[System.Windows.Forms.SendKeys]::SendWait("^+5"); Start-Sleep -Seconds 2
# Zeilen: .., m02 … m12 (11), m13 …, Unter\x.txt
[W]::SetCursorPos($mr.L + 40, (RowY 13)) | Out-Null; Start-Sleep -Milliseconds 200
[W]::mouse_event(0x0020, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
[W]::mouse_event(0x0040, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Seconds 2
Shot "screenshot35.png"   # nach mittlerer Maustaste: Verzeichnis „Unter“, x.txt markiert
$p.Refresh()
$mTitle = $p.MainWindowTitle
$selCount3 = ([W]::SendMessage($mList, 0x1032, [IntPtr]::Zero, [IntPtr]::Zero)).ToInt32()
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
$midOk = ($mTitle -match 'Liste\\Unter$') -and $selCount3 -eq 1
$mInfo = "Rahmen markiert: $selCount1 (erwartet 5); schmaler Rahmen: $selCount2 (erwartet 1); Ziehen auf dem Namen verschiebt m01: $moved; mittlere Maustaste: Titel [$mTitle], markiert $selCount3"
if ($selCount1 -eq 5 -and $selCount2 -eq 1 -and $moved -and $midOk) { Write-Output "::notice title=Markieren durch Ziehen::$mInfo" }
else {
  Write-Output "::error title=Markieren durch Ziehen::$mInfo"
  "Markieren durch Ziehen bzw. mittlere Maustaste nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}

# ---- Kopieren einer einzelnen Datei, deren Name im Ziel vorhanden ist: „Mit neuem Namen ablegen“ (vorbelegt „x (1).txt“) ----
$cfLog = "$PWD\konflikt-log.txt"
trap {
  $m = "Ausnahme in Zeile $($_.InvocationInfo.ScriptLineNumber): $($_.Exception.Message)"
  $m | Out-File -FilePath $cfLog -Append -Encoding UTF8
  Write-Output "::error title=GUI-Test (Ausnahme)::$m"
  break
}
$cfBase = "C:\QFilesKonflikt"
Remove-Item $cfBase -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$cfBase\Quelle", "$cfBase\Ziel" | Out-Null
Set-Content -Path "$cfBase\Quelle\x.txt" -Value "neu" -Encoding UTF8
Set-Content -Path "$cfBase\Ziel\x.txt" -Value "alt" -Encoding UTF8
$script:p = Start-Process -FilePath $Exe -ArgumentList ('"' + "$cfBase\Quelle" + '"'), ('"' + "$cfBase\Ziel" + '"') -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start (Namenskonflikt)"
# Hauptfenster über die Fensterklasse suchen (Process.MainWindowHandle kann ein Tooltip-Fenster sein)
$mainWnd = [IntPtr]::Zero
for ($t = 0; $t -lt 40 -and $mainWnd -eq [IntPtr]::Zero; $t++) { $mainWnd = [W]::FindWindow("QFilesMain", [NullString]::Value); if ($mainWnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 } }
[W]::SetForegroundWindow($mainWnd) | Out-Null
$panes = @()
$h = [IntPtr]::Zero
while ($true) {
  $h = [W]::FindWindowEx($mainWnd, $h, "QFilesPane", $null)
  if ($h -eq [IntPtr]::Zero) { break }
  if (-not [W]::IsWindowVisible($h)) { continue }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $panes += [pscustomobject]@{ H = $h; L = $r.L; T = $r.T }
}
$panes = @($panes | Sort-Object T, L)
"Hauptfenster: $mainWnd; Listen: $($panes.Count)" | Out-File -FilePath $cfLog -Append -Encoding UTF8
if ($panes.Count -lt 2) {
  Shot "screenshot25.png"
  $cls = New-Object System.Text.StringBuilder 64; [W]::GetClassName($mainWnd, $cls, 64) | Out-Null
  "Fensterklasse: $cls; Titel: $($p.MainWindowTitle)" | Out-File -FilePath $cfLog -Append -Encoding UTF8
  Write-Output "::error title=Namenskonflikt::Listenfenster nicht gefunden (Hauptfenster $mainWnd, Klasse $cls, Titel $($p.MainWindowTitle))"
  "Namenskonflikt: Listen nicht gefunden" | Tee-Object -FilePath smoke-log.txt; exit 1
}
$cfList = [W]::FindWindowEx($panes[0].H, [IntPtr]::Zero, "SysListView32", $null)
$cr = New-Object W+RECT; [W]::GetWindowRect($cfList, [ref]$cr) | Out-Null
[W]::Click($cr.L + 40, $cr.T + 24 + 1 * 20 + 10, $false); Start-Sleep -Milliseconds 500   # x.txt (nach „..“)
[System.Windows.Forms.SendKeys]::SendWait("+{F5}")
$box = [IntPtr]::Zero
for ($t = 0; $t -lt 20 -and $box -eq [IntPtr]::Zero; $t++) { $box = [W]::FindWindow("#32770", "Kopieren"); if ($box -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 } }
if ($box -ne [IntPtr]::Zero) { [System.Windows.Forms.SendKeys]::SendWait("{ENTER}") }
$cdlg = [IntPtr]::Zero
for ($t = 0; $t -lt 20 -and $cdlg -eq [IntPtr]::Zero; $t++) { $cdlg = [W]::FindWindow("#32770", "Kopieren – Name vorhanden"); if ($cdlg -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 } }
$cfSeen = $cdlg -ne [IntPtr]::Zero
"Eingabefeld Kopieren: $($box -ne [IntPtr]::Zero); Konfliktdialog: $cfSeen; Listen: $($panes.Count)" | Out-File -FilePath $cfLog -Append -Encoding UTF8
if ($cfSeen) {
  Shot "screenshot25.png"
  [W]::SendMessage([W]::GetDlgItem($cdlg, 110), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # „Mit neuem Namen ablegen“
  Start-Sleep -Seconds 3
}
Check-Alive "Namenskonflikt"
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
$cfNew = Test-Path "$cfBase\Ziel\x (1).txt"
$cfOld = ((Get-Content "$cfBase\Ziel\x.txt" -Raw) -match 'alt')
$cfInfo = "Dialog: $cfSeen; x (1).txt angelegt: $cfNew; vorhandene Datei unverändert: $cfOld"
if ($cfSeen -and $cfNew -and $cfOld) { Write-Output "::notice title=Namenskonflikt::$cfInfo" }
else {
  Write-Output "::error title=Namenskonflikt::$cfInfo"
  "Namenskonflikt beim Kopieren nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}

# ---- Programmdateien (F11 auf .exe/.dll: Eigenschaften mit Symbol) und „Mit anderer Liste vergleichen“
#      (Beyond Compare über QFILES_BCOMPARE vorgetäuscht; das Kontextmenü wird nur gezeigt, nicht ausgeführt) ----
$prgA = "C:\QFilesProgramme"; $prgB = "C:\QFilesProgramme2"
New-Item -ItemType Directory -Force -Path $prgA, $prgB | Out-Null
foreach ($f in "cmd.exe", "explorer.exe", "kernel32.dll") {
  $src = if ($f -eq "explorer.exe") { "$env:SystemRoot\explorer.exe" } else { "$env:SystemRoot\System32\$f" }
  Copy-Item $src "$prgA\$f" -Force
}
Copy-Item "$env:SystemRoot\System32\cmd.exe" "$prgB\cmd.exe" -Force
Remove-Item $viewLog -ErrorAction SilentlyContinue
$env:QFILES_TEST_VIEWERLOG = $viewLog
$env:QFILES_BCOMPARE = "$env:SystemRoot\System32\cmd.exe"
$script:p = Start-Process -FilePath $Exe -ArgumentList ('"' + $prgA + '"'),('"' + $prgB + '"') -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start (Programme)"
$mainH = [W]::FindWindow("QFilesMain", [NullString]::Value)
[W]::SetForegroundWindow($mainH) | Out-Null
$best = [IntPtr]::Zero; $bestKey = [int]::MaxValue; $h = [IntPtr]::Zero
while ($true) {
  $h = [W]::FindWindowEx($mainH, $h, "QFilesPane", $null)
  if ($h -eq [IntPtr]::Zero) { break }
  if (-not [W]::IsWindowVisible($h)) { continue }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $key = $r.L + $r.T * 10
  if ($key -lt $bestKey) { $bestKey = $key; $best = $h }
}
$plist = [W]::FindWindowEx($best, [IntPtr]::Zero, "SysListView32", $null)
$prc = New-Object W+RECT; [W]::GetWindowRect($plist, [ref]$prc) | Out-Null
[W]::Click($prc.L + 60, $prc.T + 24 + 1 * 20 + 10, $false); Start-Sleep -Milliseconds 800
[System.Windows.Forms.SendKeys]::SendWait("{HOME}{DOWN}"); Start-Sleep -Milliseconds 400
[W]::Key(0x5D); Start-Sleep -Seconds 2   # Kontextmenü von cmd.exe: mit Eintrag „Mit anderer Liste vergleichen“
Shot "screenshot26.png"
[System.Windows.Forms.SendKeys]::SendWait("{ESC}"); Start-Sleep -Milliseconds 600
Remove-Item Env:\QFILES_BCOMPARE
[System.Windows.Forms.SendKeys]::SendWait("{F11}")
Wait-ViewerLog 1
Check-Alive "Dateianzeige Programm 1"
Shot "screenshot27.png"   # cmd.exe: Symbol, Versionsinfo
for ($i = 1; $i -le 2; $i++) {
  [System.Windows.Forms.SendKeys]::SendWait("{DOWN}")
  Wait-ViewerLog ($i + 1)
  Check-Alive "Dateianzeige Programm $($i + 1)"
}
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item Env:\QFILES_TEST_VIEWERLOG
$pgl = @()
if (Test-Path $viewLog) { $pgl = @(Get-Content $viewLog -Encoding UTF8) }
$pgl | ForEach-Object { Write-Output $_ }
$pgOk = @($pgl | Where-Object { $_ -match '^(cmd\.exe|explorer\.exe|kernel32\.dll) \| Programm \| \d+ Zeilen \|.*Format=PE32\+' }).Count
$pgIcon = @($pgl | Where-Object { $_ -match '^(cmd|explorer)\.exe \| Programm \|.*Symbol ja' }).Count
if ($pgOk -eq 3 -and $pgIcon -eq 2) { Write-Output ("::notice title=Programmdateien::" + ($pgl -join ' / ')) }
else {
  Write-Output ("::error title=Programmdateien::" + ($pgl -join ' / '))
  "Eigenschaften von Programmdateien nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}
if (@($pgl | Where-Object { $_ -match 'Signatur=gültig' }).Count -lt 3) {
  Write-Output "::warning title=Signaturprüfung::Nicht alle Systemdateien als gültig signiert erkannt"
}

# ---- Papierkorb: Datei in den Papierkorb löschen, in der Ansicht „Papierkorb“ (Liste 1) finden und über das
#      Kontextmenü wiederherstellen ----
$binDir = "C:\QFilesPapierkorb"
New-Item -ItemType Directory -Force -Path $binDir | Out-Null
$binName = "weg ✓ " + (Get-Random -Maximum 99999) + ".txt"
$binFile = Join-Path $binDir $binName
Set-Content -Path $binFile -Value "Inhalt für den Papierkorb" -Encoding UTF8
Add-Type -AssemblyName Microsoft.VisualBasic
[Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($binFile, 'OnlyErrorDialogs', 'SendToRecycleBin')
$binDeleted = -not (Test-Path $binFile)
$binLog = "$PWD\papierkorb-log.txt"
Remove-Item $binLog -ErrorAction SilentlyContinue
$env:QFILES_TEST_PANELOG = $binLog
$warmLog = "$PWD\warmup-log.txt"
Remove-Item $warmLog -ErrorAction SilentlyContinue
$env:QFILES_TEST_WARMUPLOG = $warmLog
$script:p = Start-Process -FilePath $Exe -ArgumentList 'Papierkorb',('"' + $binDir + '"') -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start (Papierkorb)"
$mainH = [W]::FindWindow("QFilesMain", [NullString]::Value)
[W]::SetForegroundWindow($mainH) | Out-Null
$best = [IntPtr]::Zero; $bestKey = [int]::MaxValue; $h = [IntPtr]::Zero
while ($true) {
  $h = [W]::FindWindowEx($mainH, $h, "QFilesPane", $null)
  if ($h -eq [IntPtr]::Zero) { break }
  if (-not [W]::IsWindowVisible($h)) { continue }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $key = $r.L + $r.T * 10
  if ($key -lt $bestKey) { $bestKey = $key; $best = $h }
}
$blist = [W]::FindWindowEx($best, [IntPtr]::Zero, "SysListView32", $null)
$brc = New-Object W+RECT; [W]::GetWindowRect($blist, [ref]$brc) | Out-Null
[W]::Click($brc.L + 60, $brc.T + 24 + 10, $false); Start-Sleep -Milliseconds 600
[System.Windows.Forms.SendKeys]::SendWait("{HOME}"); Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait("weg"); Start-Sleep -Milliseconds 800   # Tippen springt zum Namen
Shot "screenshot28.png"   # Papierkorb-Ansicht (Name, ursprünglicher Ort, gelöscht am)
[W]::Key(0x5D); Start-Sleep -Seconds 1
Shot "screenshot29.png"   # Kontextmenü des Papierkorbs
[System.Windows.Forms.SendKeys]::SendWait("w"); Start-Sleep -Seconds 3   # Wiederherstellen
Check-Alive "Papierkorb wiederherstellen"
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item Env:\QFILES_TEST_PANELOG
Remove-Item Env:\QFILES_TEST_WARMUPLOG
if (Test-Path $warmLog) { Write-Output ("::notice title=Kontextmenü vorbereitet::" + ((Get-Content $warmLog -Encoding UTF8) -join ' / ')) }
else { Write-Output "::warning title=Kontextmenü vorbereitet::kein Protokoll" }
$bl = if (Test-Path $binLog) { (Get-Content $binLog -Encoding UTF8) -join ' / ' } else { "" }
$binListed = $bl -match [regex]::Escape("Liste 1 | Papierkorb |") -and $bl -match [regex]::Escape($binFile)
$binRestored = Test-Path $binFile
$binInfo = "gelöscht: $binDeleted; im Papierkorb gelistet: $binListed; wiederhergestellt: $binRestored"
if ($binDeleted -and $binListed -and $binRestored) { Write-Output "::notice title=Papierkorb::$binInfo" }
else {
  Write-Output "::error title=Papierkorb::$binInfo – Protokoll: $($bl.Substring(0, [Math]::Min(600, $bl.Length)))"
  "Papierkorb-Ansicht nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}

# ---- Kachelansicht: viele Bilder (Vorausladen im Hintergrund), lange Namen (Endung bleibt), Videos, HEIC mit
#      EXIF-Vorschaubild; danach „Werkzeugleiste anpassen“ (Schaltfläche „Aktualisieren“ hinzufügen) ----
$kDir = "C:\QFilesKacheln"
& python tools\make-test-thumbs.py $kDir
$thumbLog = "$PWD\miniaturen-log.txt"
Remove-Item $thumbLog -ErrorAction SilentlyContinue
$env:QFILES_TEST_THUMBLOG = $thumbLog
$script:p = Start-Process -FilePath $Exe -ArgumentList ('"' + $kDir + '"'),'"C:\Windows"' -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start (Kacheln)"
$mainH = [W]::FindWindow("QFilesMain", [NullString]::Value)
[W]::SetForegroundWindow($mainH) | Out-Null
$best = [IntPtr]::Zero; $bestKey = [int]::MaxValue; $h = [IntPtr]::Zero
while ($true) {
  $h = [W]::FindWindowEx($mainH, $h, "QFilesPane", $null)
  if ($h -eq [IntPtr]::Zero) { break }
  if (-not [W]::IsWindowVisible($h)) { continue }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $key = $r.L + $r.T * 10
  if ($key -lt $bestKey) { $bestKey = $key; $best = $h }
}
$klist = [W]::FindWindowEx($best, [IntPtr]::Zero, "SysListView32", $null)
$krc = New-Object W+RECT; [W]::GetWindowRect($klist, [ref]$krc) | Out-Null
[W]::Click($krc.L + 60, $krc.T + 24 + 10, $false); Start-Sleep -Milliseconds 600
[System.Windows.Forms.SendKeys]::SendWait("^+4"); Start-Sleep -Seconds 2
Shot "screenshot30.png"   # Kacheln (Anfang)
# Vorausladen: ohne Blättern müssen nach kurzer Zeit alle Miniaturen geladen sein
for ($t = 0; $t -lt 60; $t++) {
  if ((Test-Path $thumbLog) -and @(Get-Content $thumbLog -Encoding UTF8).Count -ge 156) { break }
  Start-Sleep -Milliseconds 500
}
[System.Windows.Forms.SendKeys]::SendWait("{END}"); Start-Sleep -Milliseconds 800
[System.Windows.Forms.SendKeys]::SendWait("H"); Start-Sleep -Seconds 1   # zum HEIC-Bild
Shot "screenshot31.png"   # Kacheln (HEIC, Videos grau)
# Werkzeugleiste anpassen: Ansicht → Werkzeugleiste anpassen…, erste verfügbare Schaltfläche hinzufügen, OK
[System.Windows.Forms.SendKeys]::SendWait("%a"); Start-Sleep -Milliseconds 600
[System.Windows.Forms.SendKeys]::SendWait("p"); Start-Sleep -Seconds 2
Shot "screenshot32.png"   # Dialog „Werkzeugleiste anpassen“
[System.Windows.Forms.SendKeys]::SendWait("%h"); Start-Sleep -Milliseconds 600
[System.Windows.Forms.SendKeys]::SendWait("{ENTER}"); Start-Sleep -Seconds 2
Shot "screenshot33.png"   # Werkzeugleiste mit „Aktualisieren“
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item Env:\QFILES_TEST_THUMBLOG
$tl = @()
if (Test-Path $thumbLog) { $tl = @(Get-Content $thumbLog -Encoding UTF8) }
$byHow = ($tl | ForEach-Object { ($_ -split ' \| ')[1] } | Group-Object | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ', '
$heic = ($tl | Where-Object { $_ -match '^Handyfoto\.heic' }) -join ''
$iniNow = Get-Content $ini -Raw -Encoding UTF8
$tbOk = $iniNow -match 'Schaltflaechen=[^\r\n]*Aktualisieren'
$kInfo = "Miniaturen: $($tl.Count) von 156 ($byHow); HEIC: $heic; Werkzeugleiste angepasst: $tbOk"
if ($tl.Count -ge 156 -and $tbOk) { Write-Output "::notice title=Kacheln/Werkzeugleiste::$kInfo" }
else {
  Write-Output "::error title=Kacheln/Werkzeugleiste::$kInfo"
  "Kachelansicht bzw. Werkzeugleiste nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}
if ($heic -notmatch '\| (Shell|WIC|EXIF)$') { Write-Output "::warning title=HEIC::Keine Miniatur für HEIC ($heic)" }

# ---- Favoriten und privater Modus: Liste 1 zeigt die Favoriten (hellgelb, Spalte „Verzeichnis“), eine nicht mehr
#      vorhandene Datei entfällt stillschweigend; Kopfzeilen im privaten Modus grün ----
$favLog = "$PWD\favoriten-log.txt"
Remove-Item $favLog -ErrorAction SilentlyContinue
$env:QFILES_TEST_PANELOG = $favLog
Add-Content -Path $ini -Value ("`r`n[Favoriten]`r`nDatei1=C:\QFilesDemo\liesmich.txt`r`nDatei2=C:\QFilesDemo\Ünïcödé 日本語 ✓\Grüße €.txt" +
  "`r`nDatei3=C:\QFilesDemo\gibt es nicht.txt`r`n[Optionen]`r`nPrivaterModus=1") -Encoding UTF8
$script:p = Start-Process -FilePath $Exe -ArgumentList 'Favoriten','"C:\Windows"' -PassThru
Start-Sleep -Seconds 8
Check-Alive "Start (Favoriten)"
[W]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500
Shot "screenshot24.png"
$p.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Remove-Item Env:\QFILES_TEST_PANELOG
$fl = if (Test-Path $favLog) { (Get-Content $favLog -Encoding UTF8) -join ' / ' } else { "" }
$favOk = ($fl -match 'Liste 1 \| Favoriten \|') -and ($fl -match 'liesmich\.txt') -and ($fl -match 'Grüße') -and ($fl -notmatch 'gibt es nicht')
$iniText = Get-Content $ini -Raw -Encoding UTF8
$favPruned = $iniText -notmatch 'gibt es nicht\.txt'
if ($favOk -and $favPruned) { Write-Output "::notice title=Favoriten::$fl" }
else {
  Write-Output "::error title=Favoriten::Liste: $fl; fehlende Datei entfernt: $favPruned"
  "Favoriten nicht wie erwartet" | Tee-Object -FilePath smoke-log.txt; exit 1
}

if (-not (Test-Path "C:\ProgramData\QFiles\QFiles.ini")) {
  "Einstellungsdatei wurde nicht in C:\ProgramData\QFiles angelegt" | Tee-Object -FilePath smoke-log.txt; exit 1
}
Write-Output "GUI-Test bestanden; Einstellungsdatei: C:\ProgramData\QFiles\QFiles.ini"
exit 0
