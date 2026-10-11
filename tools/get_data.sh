#!/bin/bash
# Tyrian 2.1 data -> data/   (the freeware release, as OpenTyrian uses it)
#   tools/get_data.sh [tyrian21.zip]
# Downloads tyrian21.zip (or uses the one given), checks its SHA-256 and unpacks
# it into data/. The data is not part of this source; build.sh runs this when
# data/ is missing.
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SHA=7790d09a2a3addcd33c66ef063d5900eb81cc9c342f4807eb8356364dd1d9277
URLS="https://www.camanis.net/tyrian/tyrian21.zip https://mirror.amdmi3.ru/distfiles/tyrian21.zip"
ZIP="${1:-$HERE/build/tyrian21.zip}"
mkdir -p "$HERE/build"
if [ ! -f "$ZIP" ]; then
  for u in $URLS; do
    echo "    downloading $u"
    if command -v curl >/dev/null; then curl -fsSL -o "$ZIP" "$u" && break
    else wget -q -O "$ZIP" "$u" && break; fi
    rm -f "$ZIP"
  done
  [ -f "$ZIP" ] || { echo "*** could not download tyrian21.zip - put it in build/ (or pass its path) and run again"; exit 1; }
fi
echo "$SHA  $ZIP" | sha256sum -c --quiet - || { echo "*** $ZIP is not the Tyrian 2.1 freeware zip (SHA-256 differs)"; exit 1; }
TMP="$HERE/build/tyrian21_unzip"
rm -rf "$TMP" && mkdir -p "$TMP"
python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$ZIP" "$TMP"
mkdir -p "$HERE/data"
cp -a "$TMP"/tyrian21/. "$HERE/data/"
rm -rf "$TMP"
echo "    Tyrian 2.1 data in $HERE/data ($(ls "$HERE/data" | wc -l) files)"
