#!/bin/sh
# Builds out/Frond-VERSION-windows-x64.zip and, with Inno Setup, out/Frond-VERSION-setup.exe, in MSYS2 UCRT64.
# Run from the repository root after out/frond.exe, out/frond-68k.exe and build/desktop/frond-desktop.exe exist.
set -eu
for f in out/frond.exe out/frond-68k.exe build/desktop/frond-desktop.exe; do
    [ -f "$f" ] || { echo "make-package: $f is missing, build it first"; exit 1; }
done

VERSION=$(head -n 1 VERSION)
DIR=out/package/Frond  # not out/Frond: MSYS would match out/frond.exe
rm -rf "$DIR"
mkdir -p "$DIR"
cp build/desktop/frond-desktop.exe "$DIR/frond-desktop.exe"
cp out/frond.exe out/frond-68k.exe "$DIR/"
strip "$DIR/frond.exe" "$DIR/frond-68k.exe" "$DIR/frond-desktop.exe" || true
cp LICENSE README.md "$DIR/" 2>/dev/null || true
[ -d licences ] && cp -R licences "$DIR/licences"
# Qt's DLLs and plugins.
windeployqt6 --no-translations "$DIR/frond-desktop.exe"
# The libraries the cores and the desktop pull in from UCRT64 (SDL2, libslirp, zlib, the C++ runtime...).
python3 packaging/windows/bundle-dlls.py "$DIR" "$DIR/frond-desktop.exe" "$DIR/frond.exe" "$DIR/frond-68k.exe"
ZIP="out/Frond-$VERSION-windows-x64.zip"
rm -f "$ZIP"
python3 - "$DIR" "$ZIP" <<'PY'
import os, sys, zipfile
src, dest = sys.argv[1:3]
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(src):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.join("Frond", os.path.relpath(p, src)))
PY
echo "$ZIP"
ISCC=""
for c in "$(command -v iscc 2>/dev/null || true)" "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" "/c/Program Files/Inno Setup 6/ISCC.exe"; do
    if [ -n "$c" ] && [ -x "$c" ]; then ISCC=$c; break; fi
done
if [ -n "$ISCC" ]; then
    export MSYS2_ARG_CONV_EXCL="*"  # keep /D... as they are
    "$ISCC" "/DAppVersion=$VERSION" "/DSourceDir=$(cygpath -w "$PWD/$DIR")" "/DOutputDir=$(cygpath -w "$PWD/out")" \
        "$(cygpath -w "$PWD/packaging/windows/frond.iss")"
    echo "out/Frond-$VERSION-setup.exe"
else
    echo "Inno Setup not found; no installer" >&2
    exit 1
fi
