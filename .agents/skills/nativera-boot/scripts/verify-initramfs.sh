#!/bin/bash
# Verify an initramfs archive is actually bootable, without booting it.
#
# Usage: ./verify-initramfs.sh <initramfs.cpio.gz>
#
# Checks the three things that have silently broken this project:
#   1. the busybox is a full build, not the stripped 147-applet initcpio one
#   2. every applet init calls actually exists
#   3. the mountpoints exist as directories inside the archive
#   4. /init is present and executable
#
# Exit 0 = all checks pass. Exit 1 = at least one failed, with the reason.

set -uo pipefail

ARCHIVE="${1:-}"
if [ -z "$ARCHIVE" ] || [ ! -f "$ARCHIVE" ]; then
    echo "usage: $0 <initramfs.cpio.gz>" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FAIL=0
fail() { printf '  FAIL  %s\n' "$*"; FAIL=1; }
pass() { printf '  ok    %s\n' "$*"; }

echo "==> archive: $ARCHIVE ($(stat -c %s "$ARCHIVE") bytes)"

# --- unpack ------------------------------------------------------------------
if ! gzip -dc "$ARCHIVE" | ( cd "$WORK" && cpio -idm --quiet 2>/dev/null ); then
    echo "  FAIL  could not unpack (not gzip+newc cpio?)" >&2
    exit 1
fi
echo "  ok    unpacks ($(find "$WORK" | wc -l) entries)"

# --- busybox -----------------------------------------------------------------
BB="$WORK/bin/busybox"
if [ ! -x "$BB" ]; then
    fail "/bin/busybox missing or not executable"
else
    COUNT="$("$BB" --list 2>/dev/null | wc -l)"
    if [ "$COUNT" -lt 300 ]; then
        fail "busybox has only $COUNT applets - this is the stripped initcpio build"
        echo "        use flashdrive/vendor/busybox-static (392 applets)"
    else
        pass "busybox has $COUNT applets"
    fi
    if file -b "$BB" | grep -q "statically linked"; then
        pass "busybox is statically linked"
    else
        pass "busybox is dynamically linked (needs libc in the archive)"
    fi

    # --- applets init depends on ---------------------------------------------
    APPLETS="$("$BB" --list 2>/dev/null)"
    MISSING=""
    for a in sh mount umount mknod mkdir cat echo ls cp rm ln sleep dmesg head \
             tail cut wc date env chmod chown free clear kill ps pwd mv rmdir \
             uname awk sed sort tr find printf test true false tee sync \
             cttyhack setsid; do
        printf '%s\n' "$APPLETS" | grep -qx -- "$a" || MISSING="$MISSING $a"
    done
    if [ -n "$MISSING" ]; then
        fail "missing applets:$MISSING"
    else
        pass "all required applets present"
    fi
fi

# --- mountpoints -------------------------------------------------------------
MP_MISSING=""
for d in proc sys dev run; do
    [ -d "$WORK/$d" ] || MP_MISSING="$MP_MISSING /$d"
done
if [ -n "$MP_MISSING" ]; then
    fail "mountpoints missing from archive:$MP_MISSING"
    echo "        mount -t proc none /proc fails silently without these"
else
    pass "mountpoints /proc /sys /dev /run exist"
fi

# --- init --------------------------------------------------------------------
if [ ! -f "$WORK/init" ]; then
    fail "/init missing"
elif [ ! -x "$WORK/init" ]; then
    fail "/init is not executable"
else
    pass "/init present and executable"
    # Strip comments before matching: init documents the broken pattern in a
    # comment, and a naive grep flags the documentation as the bug.
    INIT_CODE="$(grep -v '^[[:space:]]*#' "$WORK/init")"
    if printf '%s\n' "$INIT_CODE" | grep -q 'exec cttyhack'; then
        fail "init uses 'exec cttyhack' - PID 1 cannot get a controlling tty"
        echo "        use: setsid cttyhack /bin/sh"
    elif printf '%s\n' "$INIT_CODE" | grep -q 'setsid cttyhack'; then
        pass "init uses 'setsid cttyhack' (correct for PID 1)"
    else
        echo "  note  init has no cttyhack call - job control may be off"
    fi
fi

# --- console order in the deployed config ------------------------------------
for cfg in "$WORK/limine.conf" "$WORK/EFI/BOOT/limine.conf"; do
    [ -f "$cfg" ] || continue
    CMDLINE="$(grep -o 'cmdline:.*' "$cfg" | head -1)"
    if [ -n "$CMDLINE" ]; then
        LAST="$(printf '%s\n' "$CMDLINE" | tr ' ' '\n' | grep '^console=' | tail -1)"
        case "$LAST" in
            console=ttyS*|console=ttyUSB*)
                fail "$(basename "$cfg"): last console= is $LAST - /dev/console goes to serial"
                echo "        reorder so console=tty0 is LAST"
                ;;
            console=tty0)
                pass "$(basename "$cfg"): /dev/console -> tty0 (correct)"
                ;;
        esac
    fi
done

echo
if [ "$FAIL" -eq 0 ]; then
    echo "==> PASS"
    exit 0
else
    echo "==> FAIL - do not deploy this archive"
    exit 1
fi
