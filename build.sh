#!/bin/sh
# Domdly build script for Linux (and the portable core on macOS / other POSIX
# systems). Everything goes to build/linux/.
#
# Usage: ./build.sh [app|test|deb|clean]
#   app    (default) core tests, md2html and the GTK 3 app build/linux/domdly
#   test   core tests only
#   deb    the app, then a Debian package in build/linux/ (dpkg-deb)
#   clean  remove build/linux
#
# Debian packages needed for the app: build-essential pkg-config libgtk-3-dev
set -eu

cd "$(dirname "$0")"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2 -Wall -Wextra}"
OUT=build/linux
CORE="src/core/mdbuf.c src/core/markdown.c src/core/render_html.c src/core/render_rtf.c src/core/md_write.c src/core/md_html.c"
VERSION=$(sed -n 's/^#define DOMDLY_VERSION_STR *"\(.*\)"/\1/p' src/version.h)
MODE="${1:-app}"

if [ "$MODE" = "clean" ]; then
    rm -rf "$OUT"
    echo "Cleaned."
    exit 0
fi

mkdir -p "$OUT"

echo "[1/4] Building and running core tests..."
# shellcheck disable=SC2086
$CC -std=c99 -Wpedantic $CFLAGS -Isrc tests/test_markdown.c $CORE -o "$OUT/test_markdown"
"./$OUT/test_markdown"
[ "$MODE" = "test" ] && exit 0

echo "[2/4] Building md2html..."
# shellcheck disable=SC2086
$CC -std=c99 -Wpedantic $CFLAGS -Isrc tools/md2html.c $CORE -o "$OUT/md2html"

echo "[3/4] Drawing the icon..."
# shellcheck disable=SC2086
$CC -std=c99 $CFLAGS tools/mkicon.c -lm -o "$OUT/mkicon"
"./$OUT/mkicon" --png 256 "$OUT/domdly.png"

if ! pkg-config --exists gtk+-3.0 2>/dev/null; then
    echo "GTK 3 development files not found (Debian: sudo apt install libgtk-3-dev pkg-config)."
    echo "Built the core tools only: $OUT/md2html"
    [ "$MODE" = "deb" ] && exit 1
    exit 0
fi

echo "[4/4] Building domdly (GTK 3)..."
# shellcheck disable=SC2046,SC2086
$CC -std=c11 $CFLAGS -Isrc src/platform/gtk/main.c $CORE $(pkg-config --cflags --libs gtk+-3.0) -lm -o "$OUT/domdly"
echo "Done: $OUT/domdly"

if [ "$MODE" = "deb" ]; then
    ARCH=$(dpkg --print-architecture)
    PKG="$OUT/pkg"
    rm -rf "$PKG"
    mkdir -p "$PKG/DEBIAN" "$PKG/usr/bin" "$PKG/usr/share/applications" "$PKG/usr/share/doc/domdly" \
             "$PKG/usr/share/icons/hicolor/256x256/apps"
    install -m 755 "$OUT/domdly" "$PKG/usr/bin/domdly"
    install -m 755 "$OUT/md2html" "$PKG/usr/bin/domdly-md2html"
    install -m 644 "$OUT/domdly.png" "$PKG/usr/share/icons/hicolor/256x256/apps/domdly.png"
    install -m 644 packaging/linux/domdly.desktop "$PKG/usr/share/applications/domdly.desktop"
    install -m 644 LICENSE "$PKG/usr/share/doc/domdly/copyright"
    DEPS="libgtk-3-0 | libgtk-3-0t64"
    SIZE=$(du -sk "$PKG/usr" | cut -f1)
    cat > "$PKG/DEBIAN/control" <<EOF
Package: domdly
Version: $VERSION
Section: editors
Priority: optional
Architecture: $ARCH
Depends: $DEPS
Installed-Size: $SIZE
Maintainer: Domdly contributors <https://github.com/dogukansahil/Domdly>
Homepage: https://github.com/dogukansahil/Domdly
Description: small, fast Markdown reader and editor
 Domdly opens Markdown files in tabs, shows them formatted or as source
 (Ctrl+E switches), and saves them as Markdown, PDF or HTML. Written in
 plain C with no bundled libraries.
EOF
    dpkg-deb --root-owner-group --build "$PKG" "$OUT/domdly_${VERSION}_${ARCH}.deb" >/dev/null
    rm -rf "$PKG"
    echo "Package: $OUT/domdly_${VERSION}_${ARCH}.deb"
fi
