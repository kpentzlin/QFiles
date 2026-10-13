# Erzeugt Testarchive für die Archivliste der Dateianzeige (F11) im GUI-Test.
# Aufruf: python tools/make-test-archives.py <Zielverzeichnis>
#
# a-Projekt.zip   – ZIP mit Unterverzeichnissen, Unicode-Namen, „gespeichert“ und „Deflate“
# b-Quellen.tar.gz – dasselbe als TAR/GZ (wird über tar.exe gelesen)
# c-geheim.zip    – kennwortgeschützt (ZipCrypto, mit zip -P erzeugt)
# d-kaputt.zip    – kein gültiges Archiv
# e-Projekt.rar   – RAR5 (Verfahren „Speichern“, selbst geschrieben), wird über tar.exe gelesen
#
# Alle Archive enthalten eine Datei vom 16. Mai 2010: tar.exe schreibt Monatsnamen in der Sprache des Benutzers
# („Mai“ statt „May“), das muss QFiles erkennen.
import base64
import struct
import time
import zlib
import io
import os
import sys
import tarfile
import zipfile

ENCRYPTED = (
    "UEsDBBQACQAIAMhDTV0DimKsEgAAADIAAAAOABwAZW5jL2dlaGVpbS50eHRVVAkAAxjszWoY7M1qdXgLAAEEAAAAAAQAAAAAteihxiQTegRh"
    "eAbF1AJz7O7LUEsHCAOKYqwSAAAAMgAAAFBLAQIeAxQACQAIAMhDTV0DimKsEgAAADIAAAAOABgAAAAAAAEAAACkgQAAAABlbmMvZ2VoZWlt"
    "LnR4dFVUBQADGOzNanV4CwABBAAAAAAEAAAAAFBLBQYAAAAAAQABAFQAAABqAAAAAAA="
)

FILES = {
    "liesmich.txt": ("Archiv-Test\n" * 120).encode(),
    "Grüße €.txt": "Grüße!".encode(),
    "Quellen/main.cpp": b"int main() { return 0; }\n" * 20,
    "Quellen/Unter/tief ✓.txt": "tief".encode(),
    "Bilder/leer.txt": b"",
    "Alt/BEIN08.TXT": ("Alte Datei vom Mai 2010\r\n" * 50).encode(),
}
OLD = "Alt/BEIN08.TXT"
OLD_TIME = (2010, 5, 16, 12, 0, 0)


def vint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def rar5_block(body, data=b""):
    """Kopf: CRC32, Größe (vint), Inhalt ab Kopftyp; danach die Daten."""
    size = vint(len(body))
    return struct.pack("<I", zlib.crc32(size + body) & 0xFFFFFFFF) + size + body + data


def make_rar5(path, files):
    out = bytearray(b"Rar!\x1a\x07\x01\x00")
    out += rar5_block(vint(1) + vint(0) + vint(0))   # Hauptkopf
    dirs = sorted({"/".join(n.split("/")[:i]) for n in files for i in range(1, n.count("/") + 1)})
    entries = [(d, None) for d in dirs] + list(files.items())
    for name, data in entries:
        t = time.mktime(OLD_TIME + (0, 0, -1)) if name == OLD else 1791876600
        nb = name.encode("utf-8")
        if data is None:   # Verzeichnis
            body = vint(2) + vint(0x0002) + vint(0) + vint(0x0001 | 0x0002) + vint(0) + vint(0x10) + struct.pack("<I", int(t))
            body += vint(0) + vint(0) + vint(len(nb)) + nb
            out += rar5_block(body)
        else:
            body = vint(2) + vint(0x0002) + vint(len(data)) + vint(0x0002 | 0x0004) + vint(len(data)) + vint(0x20)
            body += struct.pack("<I", int(t)) + struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)
            body += vint(0) + vint(0) + vint(len(nb)) + nb   # Speichern, Windows
            out += rar5_block(body, data)
    out += rar5_block(vint(5) + vint(0) + vint(0))   # Archivende
    with open(path, "wb") as f:
        f.write(out)


def main(target):
    os.makedirs(target, exist_ok=True)
    with zipfile.ZipFile(os.path.join(target, "a-Projekt.zip"), "w") as z:
        z.writestr(zipfile.ZipInfo("Bilder/", (2026, 10, 13, 9, 30, 0)), b"")
        for name, data in FILES.items():
            method = zipfile.ZIP_STORED if len(data) < 16 else zipfile.ZIP_DEFLATED
            date = OLD_TIME if name == OLD else (2026, 10, 13, 9, 30, 0)
            z.writestr(zipfile.ZipInfo(name, date), data, compress_type=method)
    with tarfile.open(os.path.join(target, "b-Quellen.tar.gz"), "w:gz", format=tarfile.PAX_FORMAT) as t:
        for name, data in FILES.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = int(time.mktime(OLD_TIME + (0, 0, -1))) if name == OLD else 1791876600
            t.addfile(info, io.BytesIO(data))
    with open(os.path.join(target, "c-geheim.zip"), "wb") as f:
        f.write(base64.b64decode(ENCRYPTED))
    with open(os.path.join(target, "d-kaputt.zip"), "wb") as f:
        f.write(b"Dies ist kein Archiv, nur Text mit ZIP-Endung.\n")
    make_rar5(os.path.join(target, "e-Projekt.rar"), FILES)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "testarchive")
