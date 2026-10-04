#!/bin/bash
# Syntaxprüfung einzelner Dateien mit MinGW (Aufruf: ./check.sh src/Datei.cpp ...)
cd "$(dirname "$0")/.."
rc=0
for f in "$@"; do
  x86_64-w64-mingw32-g++-posix -std=c++20 -fsyntax-only -DUNICODE -D_UNICODE -DNOMINMAX -DSTRICT \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000006 -DQFILES_VERSION_STRING='"0.1.0"' \
    -Isrc -Ires -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare \
    -Wno-cast-function-type "$f" || rc=1
done
exit $rc
