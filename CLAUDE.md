# Hinweise für Claude (QFiles)

## Versionsnummer

Bei jeder weiteren Änderungsrunde den letzten Teil der Versionsnummer um 1 erhöhen, solange in der Anfrage nichts
anderes verlangt wird (Vorgabe von Karl, 15.10.2026). Die Nummer steht an zwei Stellen, die zusammen geändert werden:

- `CMakeLists.txt`: `project(QFiles VERSION x.y.z …)` (daraus `QFILES_VERSION_STRING`, angezeigt unter Hilfe → Über)
- `res/QFiles.rc`: `FILEVERSION`/`PRODUCTVERSION` `x,y,z,0` und die Texte `"x.y.z.0"`

## Arbeitsweise

- Cloud-Sitzungen laufen unbeaufsichtigt: keine Rückfragen stellen, sinnvolle Auslegung wählen und im Bericht nennen.
- Berichte an Karl auf Deutsch und knapp.
- Commits enden mit den Zeilen `Co-Authored-By:` und `Claude-Session:` aus der Sitzungsvorgabe.

## Auslieferung auf Karls Rechner (Vorgabe von Karl, 05.11.2026)

- Nach jedem grünen CI-Lauf `QFiles.exe`, `QFiles.pdb` und `README.md` (Artefakt `QFiles-x64`, `gh run download`)
  sowie alle Bildschirmfotos (Artefakt `screenshot`) nach `C:\_Arbeit\__QFiles` bzw. `C:\_Arbeit\__QFiles\Screenshots`
  legen (über die Geräte-Werkzeuge; große Dateien wie die PDB ggf. über SendUserFile + fileUuid).
- Lässt sich `QFiles.exe` nicht überschreiben, weil die alte Version noch läuft: Karl kurz Bescheid geben, ihm Zeit
  zum Schließen lassen und den Download bzw. das Kopieren danach automatisch wiederholen.
