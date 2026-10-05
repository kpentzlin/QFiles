# libssh2 (unverändert übernommen)

- Version: 1.11.1 (Tag `libssh2-1.11.1`, https://github.com/libssh2/libssh2)
- Lizenz: BSD-3-Clause, siehe `COPYING`
- Übernommen: `include/` und `src/` (ohne die Kryptografie-Anbindungen für OpenSSL, mbedTLS, libgcrypt und OS/400)
- QFiles baut die Bibliothek statisch mit der Windows-Kryptografie (WinCNG, `LIBSSH2_WINCNG`) und
  ECDSA-Unterstützung (`LIBSSH2_ECDSA_WINCNG`, ab Windows 10) – siehe `CMakeLists.txt` im Hauptverzeichnis.
