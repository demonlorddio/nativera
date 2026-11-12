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
# NOTE: the host kernel is NOT what we boot; we bundle the LTS kernel.
KERNEL_VERSION="${KERNEL_VERSION:-6.18.55-2-cachyos-lts}"
KMOD="/usr/lib/modules/$KERNEL_VERSION"
BUSYBOX="${BUSYBOX:-/usr/lib/initcpio/busybox}"
LIMINE_EFI="${LIMINE_EFI:-/usr/share/limine/BOOTX64.EFI}"
DRIVE="${1:-/run/media/$(id -un)/BA88-0A7A}"

# --- sanity checks -----------------------------------------------------------
[ -x "$BUSYBOX" ]           || { echo "missing busybox: $BUSYBOX" >&2; exit 1; }
[ -f "$KMOD/vmlinuz" ]     || { echo "missing kernel: $KMOD/vmlinuz" >&2; exit 1; }
[ -f "$LIMINE_EFI" ]       || { echo "missing Limine UEFI: $LIMINE_EFI" >&2; exit 1; }
[ -d "$DRIVE" ] && [ -w "$DRIVE" ] ||
    { echo "drive not mounted/writable: $DRIVE (is it plugged in & mounted?)" >&2; exit 1; }

echo "==> build dir: $OUT"
rm -rf "$OUT"; mkdir -p "$OUT"

# --- 1. rootfs tree -----------------------------------------------------------
ROOT="$OUT/rootfs"
mkdir -p "$ROOT"/bin "$ROOT"/sbin "$ROOT"/etc "$ROOT"/root "$ROOT"/home \
         "$ROOT"/tmp "$ROOT"/var "$ROOT"/usr/bin "$ROOT"/usr/sbin \
         "$ROOT"/usr/lib "$ROOT"/usr/lib64

# busybox + the three glibc pieces its dynamic loader needs
cp "$BUSYBOX"                        "$ROOT/bin/busybox"
cp /usr/lib64/ld-linux-x86-64.so.2  "$ROOT/usr/lib64/"
cp /usr/lib/libc.so.6               "$ROOT/usr/lib/"
cp /usr/lib/libcrypt.so.2           "$ROOT/usr/lib/"
ln -s usr/lib   "$ROOT/lib"
ln -s usr/lib64 "$ROOT/lib64"

# applet symlinks
cd "$ROOT/bin"
for a in sh mount umount ls cat echo sleep mkdir modprobe insmod delmod lsmod \
         ip ifconfig reboot poweroff halt grep cp rm ln touch dmesg head tail \
         more tr cut find wc date env chmod chown mknod free clear kill ps \
         pwd mv rmdir uname; do
    ln -sf busybox "$a"
done
cd "$ROOT/sbin"
for a in insmod modprobe reboot poweroff halt; do
    ln -sf ../bin/busybox "$a"
done

install -m 755 "$SRC/rootfs/init" "$ROOT/init"
printf 'nativera\n' > "$ROOT/etc/hostname"

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

sync
echo "==> deployed to $DRIVE"
echo "==> Boot: enter UEFI setup, select the USB stick first (Secure Boot OFF)."
echo "==> Expect: Limine menu -> 'NativeRa Prototype' -> kernel -> text shell banner."
