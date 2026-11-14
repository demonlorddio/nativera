#!/bin/bash
# NativeRa - fix the USB stick so UEFI firmware will actually boot it.
#
# WHY THIS EXISTS
#   The stick was formatted as a legacy MBR/FAT32 volume:
#       sda1  29.3G  vfat  W95 FAT32 (LBA)
#   UEFI firmware only creates a "UEFI: <device>" boot-menu entry for a GPT
#   partition typed EFI System (EF00). A legacy FAT32 volume gets at most a
#   generic "USB HDD" entry, which most firmware refuses to boot. That is why
#   nothing bootable was detected.
#
#   The files on the stick were already correct (Limine 12.9.6 BOOTX64.EFI,
#   valid limine.conf, a valid bzImage kernel, a valid initramfs). Only the
#   partition table was wrong.
#
# WHAT THIS DOES  (DESTRUCTIVE - wipes the stick)
#   1. unmounts the stick
#   2. writes a fresh GPT label
#   3. creates one partition spanning the disk, typed EFI System (EF00), FAT32
#   4. formats it FAT32
#   5. remounts it
#   6. re-runs flashdrive/build.sh to deploy the boot files
#
# USAGE
#   sudo ./flashdrive/fix-partition.sh /dev/sda
#
#   Confirm the device first with:  lsblk -o NAME,SIZE,FSTYPE,LABEL,PARTTYPENAME,MOUNTPOINT
#   It must be the ~29.3G USB stick, NOT your nvme system disk.

set -euo pipefail

DEV="${1:-}"
if [ -z "$DEV" ]; then
    echo "usage: sudo $0 /dev/sdX" >&2
    echo >&2
    echo "current block devices:" >&2
    lsblk -o NAME,SIZE,FSTYPE,LABEL,PARTTYPENAME,MOUNTPOINT >&2
    exit 1
fi

# --- safety: refuse anything that is not a removable USB disk ---------------
if [ ! -b "$DEV" ]; then
    echo "not a block device: $DEV" >&2
    exit 1
fi

REAL="$(readlink -f "$DEV")"
TRAN="$(lsblk -no TRAN "$REAL" 2>/dev/null | head -1 | tr -d ' ')"
SIZE="$(lsblk -dno SIZE "$REAL" | tr -d ' ')"

echo "==> target: $REAL  ($SIZE, transport: ${TRAN:-unknown})"
if [ "$TRAN" != "usb" ]; then
    echo >&2
    echo "REFUSING: $REAL is not a USB device (transport='${TRAN:-unknown}')." >&2
    echo "This script only touches USB sticks. Pass the right /dev/sdX." >&2
    exit 1
fi

echo
echo "!!! THIS ERASES EVERYTHING ON $REAL ($SIZE) !!!"
echo "    Type the device path again to confirm:"
read -r CONFIRM
if [ "$CONFIRM" != "$REAL" ]; then
    echo "confirmation did not match. aborted." >&2
    exit 1
fi

# --- 1. unmount every partition on it ---------------------------------------
echo "==> unmounting"
for p in $(lsblk -lno NAME "$REAL" | tail -n +2); do
    if mountpoint -q "/dev/$p" 2>/dev/null || findmnt -S "/dev/$p" >/dev/null 2>&1; then
        umount "/dev/$p" || true
    fi
done
# udisks may hold it; try a lazy unmount as a fallback
for p in $(lsblk -lno NAME "$REAL" | tail -n +2); do
    findmnt -S "/dev/$p" >/dev/null 2>&1 && umount -l "/dev/$p" || true
done

# --- 2+3. GPT label + EFI System partition ----------------------------------
echo "==> writing GPT with an EFI System (EF00) partition"
parted -s "$REAL" mklabel gpt
parted -s "$REAL" mkpart ESP fat32 1MiB 100%
parted -s "$REAL" set 1 esp on

PART="${REAL}1"
# nvme-style naming safety (sdX -> sdX1, nvme0n1 -> nvme0n1p1)
case "$REAL" in
    *nvme*|*mmcblk*) PART="${REAL}p1" ;;
esac

echo "==> partition table now:"
parted -s "$REAL" print

# --- 4. format FAT32 --------------------------------------------------------
echo "==> formatting $PART as FAT32"
# mkfs.vfat is the usual name; mkfs.fat is the same tool on some distros
if command -v mkfs.vfat >/dev/null 2>&1; then
    mkfs.vfat -F 32 -n NATIVERA "$PART"
elif command -v mkfs.fat >/dev/null 2>&1; then
    mkfs.fat -F 32 -n NATIVERA "$PART"
else
    echo "no mkfs.vfat / mkfs.fat found. install dosfstools." >&2
    exit 1
fi

# --- 5. remount -------------------------------------------------------------
echo "==> remounting"
mkdir -p /run/media/"$(id -un)"/NATIVERA
mount "$PART" /run/media/"$(id -un)"/NATIVERA
chown "$(id -un):$(id -gn)" /run/media/"$(id -un)"/NATIVERA

echo
echo "==> partition fixed. Now deploying the boot files:"
echo "    (run as your normal user, not root)"
echo
echo "    ./flashdrive/build.sh /run/media/$(id -un)/NATIVERA"
echo
echo "==> then: sync, unmount, reboot, Secure Boot OFF, USB first."
