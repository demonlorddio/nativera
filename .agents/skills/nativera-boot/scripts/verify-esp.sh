#!/bin/bash
# Verify a deployed ESP matches the source of truth, byte for byte.
#
# Usage: ./verify-esp.sh <mountpoint>
#
# Catches the stale-deploy failure: build.sh reports success, the stick holds an
# old payload, and the boot fails on hardware after every VM test passed.
#
# Compares against:
#   - the source limine.conf
#   - the freshly built initramfs in flashdrive/build/
#   - the system Limine binary and the detected kernel
#
# Exit 0 = every file matches. Exit 1 = mismatch, with which file.

set -uo pipefail

MOUNT="${1:-}"
if [ -z "$MOUNT" ] || [ ! -d "$MOUNT" ]; then
    echo "usage: $0 <mountpoint>   e.g. /run/media/$USER/NATIVERA" >&2
    exit 1
fi

REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
SRC="$REPO/flashdrive"
BUILD="$SRC/build"

FAIL=0
fail() { printf '  FAIL  %s\n' "$*"; FAIL=1; }
pass() { printf '  ok    %s\n' "$*"; }

echo "==> mount: $MOUNT"

# --- presence and non-empty ---------------------------------------------------
for f in EFI/BOOT/BOOTX64.EFI EFI/BOOT/limine.conf limine.conf vmlinuz initramfs.cpio.gz; do
    if [ ! -s "$MOUNT/$f" ]; then
        fail "$f missing or empty"
    else
        pass "$f present ($(stat -c %s "$MOUNT/$f") bytes)"
    fi
done

# --- md5 comparison -----------------------------------------------------------
cmp_file() {
    local label="$1" deployed="$2" expected="$3"
    if [ ! -f "$expected" ]; then
        echo "  skip  $label (no reference: $expected)"
        return
    fi
    if [ ! -f "$deployed" ]; then
        fail "$label: not on media"
        return
    fi
    local a b
    a="$(md5sum "$deployed" | cut -d' ' -f1)"
    b="$(md5sum "$expected" | cut -d' ' -f1)"
    if [ "$a" = "$b" ]; then
        pass "$label matches ($a)"
    else
        fail "$label MISMATCH"
        echo "        media:    $a"
        echo "        expected: $b"
        echo "        -> stale deploy. Re-run build.sh against this mount."
    fi
}

cmp_file "limine.conf (ESP root)"  "$MOUNT/limine.conf"          "$SRC/limine.conf"
cmp_file "limine.conf (EFI/BOOT)"  "$MOUNT/EFI/BOOT/limine.conf" "$SRC/limine.conf"
cmp_file "initramfs.cpio.gz"       "$MOUNT/initramfs.cpio.gz"   "$BUILD/initramfs.cpio.gz"
cmp_file "BOOTX64.EFI"             "$MOUNT/EFI/BOOT/BOOTX64.EFI" "/usr/share/limine/BOOTX64.EFI"

# kernel: whichever version build.sh selected
KV="$(for d in /usr/lib/modules/*-lts; do [ -f "$d/vmlinuz" ] && basename "$d"; done | sort -V | tail -1)"
if [ -n "$KV" ]; then
    cmp_file "vmlinuz ($KV)" "$MOUNT/vmlinuz" "/usr/lib/modules/$KV/vmlinuz"
fi

# --- console order in the deployed config ------------------------------------
for cfg in "$MOUNT/limine.conf" "$MOUNT/EFI/BOOT/limine.conf"; do
    [ -f "$cfg" ] || continue
    LAST="$(grep -o 'cmdline:.*' "$cfg" | head -1 | tr ' ' '\n' | grep '^console=' | tail -1)"
    case "$LAST" in
        console=ttyS*|console=ttyUSB*)
            fail "$(basename "$(dirname "$cfg")")/$(basename "$cfg"): last console= is $LAST"
            echo "        /dev/console would go to serial - black screen on real hardware"
            ;;
        console=tty0)
            pass "$(basename "$(dirname "$cfg")")/$(basename "$cfg"): /dev/console -> tty0"
            ;;
    esac
done

echo
if [ "$FAIL" -eq 0 ]; then
    echo "==> PASS - media matches source. Safe to boot."
    exit 0
else
    echo "==> FAIL - do not boot this media. Fix and redeploy."
    exit 1
fi
