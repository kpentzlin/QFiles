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
