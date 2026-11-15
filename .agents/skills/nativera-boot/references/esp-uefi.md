# ESP and UEFI layout

UEFI firmware does not search a disk for a kernel. It looks for a boot entry, and
it only creates one for a partition it recognises as an EFI System Partition.

## The partition type is the whole game

A FAT32 volume is not automatically an ESP. The GPT partition type GUID must be:

```
C12A7328-F81F-11D2-BA4B-00A0C93EC93B    EFI System
```

If the partition is typed `W95 FAT32 (LBA)` (`0x0C` / MBR-style), the firmware
sees a legacy FAT volume, finds nothing it recognises, and reports no bootable
device. The files on it can be perfect.

Check:

```sh
lsblk -o NAME,SIZE,FSTYPE,LABEL,PARTTYPE,MOUNTPOINT,TRAN
```

`PARTTYPE` must be `c12a7328-f81f-11d2-ba4b-00a0c93ec93b`. `parted print` also
shows the `esp` flag.

## Creating the layout

```sh
parted -s /dev/sdX mklabel gpt
parted -s /dev/sdX mkpart ESP fat32 1MiB 100%
parted -s /dev/sdX set 1 esp on
```

Start at 1 MiB, not 0. GPT keeps a primary and backup header; starting at 0
overlaps them and some firmware refuses the disk.

## Safety for destructive operations

Any script that repartitions or formats must:

1. **Refuse non-removable media.** Check `/sys/block/<dev>/removable` and the
   transport. A script that will happily format `/dev/nvme0n1` is a data-loss
   bug waiting for a typo.
2. **Require the device path typed back.** Not `y/n` - the actual path.
3. **Print the size and transport** before doing anything, so a wrong device is
   obvious from the size.

```
==> target: /dev/sda  (29.3G, transport: usb)
!!! THIS ERASES EVERYTHING ON /dev/sda (29.3G) !!!
    Type the device path again to confirm:
```

## Required files on the ESP

```
/EFI/BOOT/BOOTX64.EFI     Limine UEFI binary (the standard removable path)
/EFI/BOOT/limine.conf     config, searched first
/limine.conf              ESP-root fallback
/vmlinuz                  kernel
/initramfs.cpio.gz        initramfs
```

`/EFI/BOOT/BOOTX64.EFI` is the path firmware tries with no boot entry present -
the "removable media" fallback. This is why the stick boots on a machine that has
never seen it.

## Limine 12 config syntax

Limine 12 changed the config format. The 5.x form is silently ignored:

```
# WRONG for 12.x - silently ignored, "config file contains no valid entries"
: NativeRa Prototype
    protocol: linux

# RIGHT for 12.x
/ NativeRa Prototype
    protocol: linux
    kernel_path: boot():/vmlinuz
    module_path: boot():/initramfs.cpio.gz
    cmdline: loglevel=7 rdinit=/init console=ttyS0 console=tty0
```

Rules:
- An entry opens with `/` followed by its title.
- Comments start with `#` and must be on their own line.
- Options follow the entry until the next entry or EOF.
- `protocol: linux` (not `limine_linux`).
- Paths use the `boot():/` prefix.
- `default_entry` is 1-based.

Verify against `/usr/share/doc/limine/CONFIG.md` on the host rather than trusting
memory - the format has changed more than once.

## Secure Boot

Limine's stock `BOOTX64.EFI` is not signed by the Microsoft UEFI CA. With Secure
Boot on, the firmware refuses it. During development: **Secure Boot off**.

Shipping to users means either signing the loader or documenting the requirement.
Do not silently assume it will work.

## Firmware quirks

- Some firmware only enumerates USB when boot mode is set to UEFI-only, not
  "UEFI and Legacy".
- Some hide USB boot behind a separate setting from the boot order.
- The one-time boot menu is usually F12, F11 or F9 depending on vendor.
- A stick that worked on one machine and not another is almost always a firmware
  setting, not a media problem. Test the same stick on both before rebuilding.

## Phase 2: three-tier layout

The single-partition model does not survive contact with a real OS. Batocera's
model, which this project is following:

| Partition | FS | Role |
|---|---|---|
| 1 | FAT32 | ESP: kernel, initramfs, bootloader, boot config |
| 2 | ext4 or squashfs | read-only root filesystem |
| 3 | ext4 | writable userdata: roms, saves, recomp packages |

The boot partition stays FAT32 because firmware requires it. The root goes
read-only squashfs so updates are atomic. Userdata stays writable and survives
updates. That is the structure to aim for; do not grow the initramfs to avoid it.
