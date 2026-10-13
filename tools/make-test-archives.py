# Erzeugt Testarchive für die Archivliste der Dateianzeige (F11) im GUI-Test.
# Aufruf: python tools/make-test-archives.py <Zielverzeichnis>
#
# a-Projekt.zip   – ZIP mit Unterverzeichnissen, Unicode-Namen, „gespeichert“ und „Deflate“
# b-Quellen.tar.gz – dasselbe als TAR/GZ (wird über tar.exe gelesen)
# c-geheim.zip    – kennwortgeschützt (ZipCrypto, mit zip -P erzeugt)
# d-kaputt.zip    – kein gültiges Archiv
import base64
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
}


def main(target):
    os.makedirs(target, exist_ok=True)
    with zipfile.ZipFile(os.path.join(target, "a-Projekt.zip"), "w") as z:
        z.writestr(zipfile.ZipInfo("Bilder/", (2026, 10, 13, 9, 30, 0)), b"")
        for name, data in FILES.items():
            method = zipfile.ZIP_STORED if len(data) < 16 else zipfile.ZIP_DEFLATED
            z.writestr(zipfile.ZipInfo(name, (2026, 10, 13, 9, 30, 0)), data, compress_type=method)
    with tarfile.open(os.path.join(target, "b-Quellen.tar.gz"), "w:gz", format=tarfile.PAX_FORMAT) as t:
        for name, data in FILES.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = 1791876600
            t.addfile(info, io.BytesIO(data))
    with open(os.path.join(target, "c-geheim.zip"), "wb") as f:
        f.write(base64.b64decode(ENCRYPTED))
    with open(os.path.join(target, "d-kaputt.zip"), "wb") as f:
        f.write(b"Dies ist kein Archiv, nur Text mit ZIP-Endung.\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "testarchive")
