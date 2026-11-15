#!/bin/bash
# NativeRa - boot the flash-drive image in VirtualBox before touching real hardware.
#
# Builds a raw GPT disk image with exactly the same layout build.sh writes to the
# USB stick (EFI System partition, FAT32, Limine + vmlinuz + initramfs), converts
# it to a VDI, boots it headless with EFI firmware, and captures the serial
# console to a log so the whole boot is readable instead of one screenshot.
#
# Usage:
#   ./flashdrive/vm-test.sh          # build image, boot, print serial log
#   ./flashdrive/vm-test.sh --keep   # leave the VM registered for inspection
#
# Requires: parted, mtools (mformat/mmd/mcopy), VBoxManage, cpio, gzip.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/flashdrive"
WORK="${WORK:-/tmp/nativera-vm}"
VM="NativeRa-Test"
IMG="$WORK/nativera.img"
VDI="$WORK/nativera.vdi"
SERIAL="$WORK/serial.log"
DISK_MB="${DISK_MB:-256}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-45}"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

# ESP starts at 1 MiB. mtools addresses the partition inside the raw image with
# the image@@offset syntax, so no loop device and no root are needed.
ESP_OFF=$((1024 * 1024))

say() { printf '==> %s\n' "$*"; }

for t in parted mformat mmd mcopy VBoxManage cpio gzip; do
    command -v "$t" >/dev/null || { echo "missing tool: $t" >&2; exit 1; }
done

# --- 1. build the boot payload (same code path as the real stick) -------------
say "building boot payload"
mkdir -p "$WORK/deploy"
if [ "${SKIP_BUILD:-0}" = "1" ]; then
    say "reusing existing payload in $WORK/deploy"
else
    "$SRC/build.sh" "$WORK/deploy" >/dev/null
fi

# --- 2. raw disk image: GPT + EFI System + FAT32 ------------------------------
say "creating ${DISK_MB}MB disk image"
rm -f "$IMG"
truncate -s "${DISK_MB}M" "$IMG"

parted -s "$IMG" mklabel gpt
parted -s "$IMG" mkpart ESP fat32 1MiB 100%
parted -s "$IMG" set 1 esp on

say "formatting ESP as FAT32"
mformat -i "$IMG@@$ESP_OFF" -F -v NATIVERA ::

# --- 3. copy the boot files into the ESP --------------------------------------
say "deploying boot files into ESP"
mmd -i "$IMG@@$ESP_OFF" ::/EFI ::/EFI/BOOT
mcopy -i "$IMG@@$ESP_OFF" "$WORK/deploy/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG@@$ESP_OFF" "$WORK/deploy/EFI/BOOT/limine.conf" ::/EFI/BOOT/limine.conf
mcopy -i "$IMG@@$ESP_OFF" "$WORK/deploy/limine.conf"        ::/limine.conf
mcopy -i "$IMG@@$ESP_OFF" "$WORK/deploy/vmlinuz"            ::/vmlinuz
mcopy -i "$IMG@@$ESP_OFF" "$WORK/deploy/initramfs.cpio.gz"  ::/initramfs.cpio.gz

say "ESP contents:"
mdir -i "$IMG@@$ESP_OFF" -/ :: | sed 's/^/    /'

# --- 4. convert to VDI --------------------------------------------------------
say "converting to VDI"
rm -f "$VDI"
VBoxManage convertfromraw "$IMG" "$VDI" --format VDI >/dev/null

# --- 5. create the VM ---------------------------------------------------------
say "creating VM $VM"
VBoxManage unregistervm "$VM" --delete >/dev/null 2>&1 || true
VBoxManage createvm --name "$VM" --ostype Linux26_64 --register >/dev/null
VBoxManage modifyvm "$VM" \
    --memory 1024 --cpus 2 --vram 16 \
    --firmware efi \
    --boot1 disk --boot2 none --boot3 none --boot4 none \
    --graphicscontroller vmsvga \
    --uart1 0x3F8 4 --uartmode1 file "$SERIAL" \
    --audio none --usb off --nic1 none >/dev/null

VBoxManage storagectl "$VM" --name SATA --add sata --controller IntelAhci >/dev/null
VBoxManage storageattach "$VM" --storagectl SATA --port 0 --device 0 \
    --type hdd --medium "$VDI" >/dev/null

# --- 6. boot headless and capture the serial console --------------------------
say "booting headless (${BOOT_TIMEOUT}s capture)"
rm -f "$SERIAL"
VBoxManage startvm "$VM" --type headless >/dev/null

for _ in $(seq 1 "$BOOT_TIMEOUT"); do
    sleep 1
    if [ -f "$SERIAL" ] && grep -qE "NativeRa|END DIAG" "$SERIAL" 2>/dev/null; then
        sleep 4   # let the diagnostics finish printing
        break
    fi
done

STATE="$(VBoxManage showvminfo "$VM" --machinereadable 2>/dev/null | grep '^VMState=' | cut -d'"' -f2)"
say "VM state: ${STATE:-unknown}"

if [ -f "$SERIAL" ]; then
    say "serial console (${SERIAL}):"
    echo "----------------------------------------"
    cat "$SERIAL"
    echo "----------------------------------------"
else
    echo "!! no serial output captured" >&2
fi

VBoxManage controlvm "$VM" poweroff >/dev/null 2>&1 || true
sleep 1

if [ "$KEEP" -eq 1 ]; then
    say "VM kept registered as $VM (VDI: $VDI)"
else
    VBoxManage unregistervm "$VM" --delete >/dev/null 2>&1 || true
    say "VM removed"
fi
