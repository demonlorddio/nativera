#!/bin/bash
# NativeRa - reproducible flash-drive build pipeline (Phase 1)
#
# Usage:
#   ./flashdrive/build.sh                 # auto-detects the drive mount
#   ./flashdrive/build.sh /mount/point    # explicit drive mount point
#
# What it does:
#   1. assembles a minimal busybox rootfs (bin/lib/etc + /init)
#   2. packages it as initramfs.cpio.gz (newc + gzip)
#   3. writes the UEFI layout to the drive:
#        EFI/BOOT/BOOTX64.EFI    (Limine UEFI)
#        EFI/BOOT/limine.conf    (searched first on EFI)
#        limine.conf             (ESP-root fallback candidate)
#        vmlinuz                 (kernel)
#        initramfs.cpio.gz
#
# No root required: everything is read from /usr and written to the
# user-mounted FAT32 drive.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/flashdrive"
OUT="$REPO/flashdrive/build"

# --- inputs (override via environment) --------------------------------------
# NOTE: the host kernel is NOT what we boot; we bundle a kernel from /usr/lib/modules.
# Auto-detect so a host kernel update can never silently break the build:
# prefer the newest *-lts module dir, else the newest module dir that has a vmlinuz.
if [ -z "${KERNEL_VERSION:-}" ]; then
    KERNEL_VERSION="$(for d in /usr/lib/modules/*-lts; do
                          [ -f "$d/vmlinuz" ] && basename "$d"
                      done | sort -V | tail -1)"
fi
if [ -z "${KERNEL_VERSION:-}" ]; then
    KERNEL_VERSION="$(for d in /usr/lib/modules/*; do
                          [ -f "$d/vmlinuz" ] && basename "$d"
                      done | sort -V | tail -1)"
fi
KMOD="/usr/lib/modules/$KERNEL_VERSION"
echo "==> kernel: ${KERNEL_VERSION:-<none found>}"

# BUSYBOX: do NOT default to /usr/lib/initcpio/busybox. That binary is a
# stripped mkinitcpio build with only 147 applets and NO mount, date, tr or
# find. Using it makes every mount in rootfs/init fail with
# "mount: applet not found", which silently leaves the shell with no /proc and
# no controlling tty. Prefer a full static busybox; verify the applets we need.
if [ -z "${BUSYBOX:-}" ]; then
    for cand in "$SRC/vendor/busybox-static" /tmp/busybox-static \
                /usr/local/bin/busybox-static /usr/bin/busybox /bin/busybox; do
        [ -x "$cand" ] && BUSYBOX="$cand" && break
    done
fi
BUSYBOX="${BUSYBOX:-/usr/lib/initcpio/busybox}"
LIMINE_EFI="${LIMINE_EFI:-/usr/share/limine/BOOTX64.EFI}"
DRIVE="${1:-/run/media/$(id -un)/BA88-0A7A}"

# --- sanity checks -----------------------------------------------------------
[ -x "$BUSYBOX" ]           || { echo "missing busybox: $BUSYBOX" >&2; exit 1; }
[ -f "$KMOD/vmlinuz" ]     || { echo "missing kernel: $KMOD/vmlinuz" >&2; exit 1; }
[ -f "$LIMINE_EFI" ]       || { echo "missing Limine UEFI: $LIMINE_EFI" >&2; exit 1; }
[ -d "$DRIVE" ] && [ -w "$DRIVE" ] ||
    { echo "drive not mounted/writable: $DRIVE (is it plugged in & mounted?)" >&2; exit 1; }

# Fail at BUILD time, not boot time, if the busybox cannot do what init needs.
# NOTE: capture the applet list ONCE into a variable. Piping `busybox --list`
# into `grep -q` makes grep exit on first match, busybox take SIGPIPE, and
# `set -o pipefail` then report a false failure for applets near the top of
# the list. That bug previously rejected a perfectly good 392-applet busybox.
echo "==> busybox: $BUSYBOX"
APPLETS="$("$BUSYBOX" --list 2>/dev/null)"
MISSING=""
for a in sh mount umount mknod mkdir cat echo ls cp rm ln sleep dmesg head tail \
         cut wc date env chmod chown free clear kill ps pwd mv rmdir uname awk \
         sed sort tr find printf test true false tee sync cttyhack; do
    printf '%s\n' "$APPLETS" | grep -qx -- "$a" || MISSING="$MISSING $a"
done
if [ -n "$MISSING" ]; then
    echo "BUSYBOX IS INCOMPLETE - missing applets:$MISSING" >&2
    echo "Point BUSYBOX= at a full static busybox (e.g. busybox.net 1.35.0 musl)." >&2
    exit 1
fi
echo "==> busybox applets: all required present"

echo "==> build dir: $OUT"
rm -rf "$OUT"; mkdir -p "$OUT"

# --- 1. rootfs tree -----------------------------------------------------------
ROOT="$OUT/rootfs"
mkdir -p "$ROOT"/bin "$ROOT"/sbin "$ROOT"/etc "$ROOT"/root "$ROOT"/home \
         "$ROOT"/tmp "$ROOT"/var "$ROOT"/usr/bin "$ROOT"/usr/sbin \
         "$ROOT"/usr/lib "$ROOT"/usr/lib64 \
         "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev "$ROOT"/run
# /proc /sys /dev /run MUST exist as mountpoints before init runs. Without them
# every mount in rootfs/init fails silently and the shell comes up with no
# /proc (empty uptime/mem) and no controlling tty.

# busybox + the three glibc pieces its dynamic loader needs
cp "$BUSYBOX"                        "$ROOT/bin/busybox"
cp /usr/lib64/ld-linux-x86-64.so.2  "$ROOT/usr/lib64/"
cp /usr/lib/libc.so.6               "$ROOT/usr/lib/"
cp /usr/lib/libcrypt.so.2           "$ROOT/usr/lib/"
ln -s usr/lib   "$ROOT/lib"
ln -s usr/lib64 "$ROOT/lib64"

# applet symlinks
# NOTE: keep this list in sync with what rootfs/init actually calls. A missing
# applet fails at runtime with "not found", not at build time.
cd "$ROOT/bin"
for a in sh mount umount ls cat echo sleep mkdir modprobe insmod delmod lsmod \
         ip ifconfig reboot poweroff halt grep cp rm ln touch dmesg head tail \
         more tr cut find wc date env chmod chown mknod free clear kill ps \
         pwd mv rmdir uname awk sed sort uniq xargs basename dirname printf \
         test true false tee sync cttyhack; do
    ln -sf busybox "$a"
done
cd "$ROOT/sbin"
for a in insmod modprobe reboot poweroff halt; do
    ln -sf ../bin/busybox "$a"
done

install -m 755 "$SRC/rootfs/init" "$ROOT/init"
printf 'nativera\n' > "$ROOT/etc/hostname"

# --- 1b. networking (optional, NET=1) ----------------------------------------
# The kernel ships EVERY ethernet driver as a module (CONFIG_E1000E=m,
# CONFIG_R8169=m, ...) and this initramfs has no /lib/modules, so without this
# block the boot has no network interface at all and nothing can load one.
#
# NET=1 adds: the common NIC modules + their firmware, libz (dropbear needs it),
# dropbear itself, and a host key generated at build time.
if [ "${NET:-0}" = "1" ]; then
    echo "==> networking: enabled"
    KVER="$KERNEL_VERSION"
    MODROOT="$ROOT/lib/modules/$KVER"
    mkdir -p "$MODROOT"

    # Copy a module and everything modules.dep says it needs.
    copy_mod() {
        local rel="$1"
        local src="/usr/lib/modules/$KVER/$rel"
        [ -f "$src" ] || { echo "    skip (absent): $rel" >&2; return 0; }
        mkdir -p "$MODROOT/$(dirname "$rel")"
        # Decompress .ko.zst -> .ko. The kernel supports compressed modules
        # (CONFIG_MODULE_DECOMPRESS=y) but busybox insmod/modprobe does NOT pass
        # the MODULE_INIT_COMPRESSED_FILE flag, so a .ko.zst fails at load with
        # "Invalid ELF header magic: != ELF". Ship plain .ko files.
        case "$rel" in
            *.ko.zst)
                unzstd -q -c "$src" > "$MODROOT/${rel%.zst}" ;;
            *)
                cp "$src" "$MODROOT/$rel" ;;
        esac
        # dependencies, from modules.dep (paths are relative to /usr/lib/modules/<ver>)
        awk -v m="$rel" 'index($0, m ":")==1 { sub(/^[^:]*: */,""); print }' \
            "/usr/lib/modules/$KVER/modules.dep" 2>/dev/null | tr ' ' '\n' | \
        while read -r dep; do
            [ -n "$dep" ] || continue
            [ -f "/usr/lib/modules/$KVER/$dep" ] || continue
            local dst="$MODROOT/${dep%.zst}"
            [ -f "$dst" ] && continue
            mkdir -p "$MODROOT/$(dirname "$dep")"
            case "$dep" in
                *.ko.zst) unzstd -q -c "/usr/lib/modules/$KVER/$dep" > "$dst" ;;
                *)        cp -n "/usr/lib/modules/$KVER/$dep" "$MODROOT/$dep" 2>/dev/null || true ;;
            esac
        done
    }

    for m in kernel/drivers/net/ethernet/intel/e1000e/e1000e.ko.zst \
             kernel/drivers/net/ethernet/intel/e1000/e1000.ko.zst \
             kernel/drivers/net/ethernet/intel/igb/igb.ko.zst \
             kernel/drivers/net/ethernet/intel/igc/igc.ko.zst \
             kernel/drivers/net/ethernet/realtek/r8169.ko.zst \
             kernel/drivers/net/phy/realtek.ko.zst \
             kernel/drivers/net/phy/mdio_devres.ko.zst \
             kernel/drivers/net/mii.ko.zst; do
        copy_mod "$m"
    done

    # modules.dep for the subset we shipped, so modprobe resolves dependencies.
    if command -v depmod >/dev/null 2>&1; then
        depmod -b "$ROOT" "$KVER" 2>/dev/null || true
    fi

    # Realtek PHY firmware: r8169 loads one of these at probe time and fails
    # without it on most RTL8111/8125 parts.
    if [ -d /usr/lib/firmware/rtl_nic ]; then
        mkdir -p "$ROOT/usr/lib/firmware/rtl_nic"
        cp /usr/lib/firmware/rtl_nic/*.zst "$ROOT/usr/lib/firmware/rtl_nic/" 2>/dev/null || true
    fi

    # libz: dropbear links against it, the base rootfs does not carry it.
    cp -L /usr/lib/libz.so.1 "$ROOT/usr/lib/libz.so.1"

    # dropbear + a host key generated now, so the image is self-contained.
    if [ -x "$SRC/vendor/dropbear/dropbear" ]; then
        install -m 755 "$SRC/vendor/dropbear/dropbear"    "$ROOT/usr/sbin/dropbear"
        install -m 755 "$SRC/vendor/dropbear/dropbearkey" "$ROOT/usr/sbin/dropbearkey"
        mkdir -p "$ROOT/etc/dropbear"
        "$SRC/vendor/dropbear/dropbearkey" -t ed25519 \
            -f "$ROOT/etc/dropbear/dropbear_ed25519_host_key" >/dev/null 2>&1
        chmod 600 "$ROOT/etc/dropbear/dropbear_ed25519_host_key"
        echo "    dropbear + host key installed"
    else
        echo "    WARNING: vendor/dropbear missing, no ssh server" >&2
    fi

    # SSH authorized_keys: baked in from authorized_keys.pub at the repo root if
    # present, so a fresh stick is immediately reachable. Root has no password,
    # so key auth is the only way in.
    mkdir -p "$ROOT/root/.ssh"
    chmod 700 "$ROOT/root/.ssh"
    if [ -f "$REPO/authorized_keys.pub" ]; then
        install -m 600 "$REPO/authorized_keys.pub" "$ROOT/root/.ssh/authorized_keys"
        echo "    ssh key: authorized_keys.pub baked in"
    else
        echo "    ssh key: none (create authorized_keys.pub to enable key login)"
    fi

    # /etc/passwd + shadow: dropbear refuses to start without a root entry.
    printf 'root:x:0:0:root:/root:/bin/sh\n' > "$ROOT/etc/passwd"
    # EMPTY password field, not '*'. A '*' marks the account as LOCKED and
    # dropbear refuses it even for publickey auth, failing with
    # "Permission denied (publickey,password)". Empty = no password set, the
    # account is usable, and dropbear runs with -w so password auth is off.
    printf 'root::19000:0:99999:7:::\n'  > "$ROOT/etc/shadow"
    chmod 600 "$ROOT/etc/shadow"
    printf 'root:x:0:\n' > "$ROOT/etc/group"

    # /etc/shells: dropbear validates the login shell from /etc/passwd against
    # this file and REJECTS the login if it is not listed. With no /etc/shells
    # at all, every login fails with "root login rejected" and "0 fails" in the
    # log - the key is never even checked. This is a required file, not optional.
    printf '/bin/sh\n/bin/ash\n/bin/bash\n' > "$ROOT/etc/shells"

    # udhcpc event script. Without this udhcpc obtains a lease and applies
    # nothing (no address, no route, no resolv.conf) and exits 0 - which looks
    # like success. See rootfs/udhcpc-default.script.
    mkdir -p "$ROOT/usr/share/udhcpc"
    install -m 755 "$SRC/rootfs/udhcpc-default.script"         "$ROOT/usr/share/udhcpc/default.script"

    # Extra applets the network path needs.
    cd "$ROOT/bin"
    for a in udhcpc nc ping route netstat wget arping; do
        ln -sf busybox "$a"
    done
    cd "$OLDPWD"

    echo "    modules: $(find "$MODROOT" -name '*.ko' | wc -l) (.ko, decompressed)"
fi

# --- 2. package: newc cpio + gzip, root-owned, symlinks preserved -----------
( cd "$ROOT" && \
  find . -print0 | cpio -o -0 -R 0:0 --format=newc 2>/dev/null | \
      gzip -9 > "$OUT/initramfs.cpio.gz" )
echo "==> initramfs: $(du -h "$OUT/initramfs.cpio.gz" | cut -f1)"

# --- 3. deploy to drive --------------------------------------------------------
mkdir -p "$DRIVE/EFI/BOOT"
cp "$LIMINE_EFI" "$DRIVE/EFI/BOOT/BOOTX64.EFI"
cp "$KMOD/vmlinuz" "$DRIVE/vmlinuz"
cp "$OUT/initramfs.cpio.gz" "$DRIVE/initramfs.cpio.gz"
cp "$SRC/limine.conf" "$DRIVE/limine.conf"
cp "$SRC/limine.conf" "$DRIVE/EFI/BOOT/limine.conf"

# --- 4. verify the deploy actually landed -------------------------------------
# A silent partial deploy is the worst failure mode: the stick looks fine and
# just does not boot. Fail loudly here instead.
echo "==> verifying deploy"
for f in EFI/BOOT/BOOTX64.EFI EFI/BOOT/limine.conf limine.conf vmlinuz initramfs.cpio.gz; do
    if [ ! -s "$DRIVE/$f" ]; then
        echo "DEPLOY FAILED: $DRIVE/$f missing or empty" >&2
        exit 1
    fi
    printf '    %-26s %10s bytes\n' "$f" "$(stat -c %s "$DRIVE/$f")"
done

sync
echo "==> deployed to $DRIVE"
echo "==> Boot: enter UEFI setup, select the USB stick first (Secure Boot OFF)."
echo "==> Expect: Limine menu -> 'NativeRa Prototype' -> kernel -> text shell banner."
