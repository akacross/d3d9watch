#!/bin/bash
# Build d3d9watch.asi (32-bit, MinGW) and its Wine test harness.
#   ./build.sh                  build into ./build
#   ./build.sh test [d3d9.dll]  build, then run the harness under Wine against a
#                               real DXVK d3d9.dll (default: the game folder's);
#                               WINEPREFIX needs Vulkan
#   ./build.sh --deploy DIR     build, then copy d3d9watch.asi into game folder DIR
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cc=i686-w64-mingw32-gcc
mkdir -p "$here/build/test"

$cc -O2 -Wall -Wextra -shared -static-libgcc -o "$here/build/d3d9watch.asi" "$here/d3d9watch.c"
cp "$here/build/d3d9watch.asi" "$here/build/test/"
$cc -O2 -Wall -o "$here/build/test/harness.exe" "$here/tests/harness.c" -luser32
echo ">> built $here/build/d3d9watch.asi"

case "${1:-}" in
  test)
    dll="${2:-/mnt/data/Games/samp-0.3.7/d3d9.dll}"
    export WINEDEBUG=-all
    wine "$here/build/test/harness.exe" "$(winepath -w "$here/build/test")" "$(winepath -w "$dll")"
    ;;
  --deploy)
    cp "$here/build/d3d9watch.asi" "${2:?--deploy needs the game folder}/"
    echo ">> deployed to $2"
    ;;
esac
