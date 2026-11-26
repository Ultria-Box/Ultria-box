#!/usr/bin/env bash
set -euo pipefail

LOWRAM="$HOME/.local/bin/lowram-run"

run() {
    if [ -x "$LOWRAM" ]; then
        "$LOWRAM" "$@"
    else
        "$@"
    fi
}

cd "$(dirname "$0")"

case "${1:-release}" in
    clean)
        rm -rf build dist
        echo "[+] cleaned"
        ;;
    debug)
        run make DEBUG=1 -j2
        ;;
    *)
        run make -j2
        ;;
esac

if [ -f dist/ultria.exe ]; then
    echo "[+] sha256: $(sha256sum dist/ultria.exe | cut -d' ' -f1)"
    echo "[+] size:   $(stat -c%s dist/ultria.exe) bytes"
fi
