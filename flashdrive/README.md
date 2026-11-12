# NativeRa Flash Drive — Phase 1 (bootable prototype)

A UEFI-bootable stick that proves the NativeRa boot chain:

```
UEFI firmware -> Limine (BOOTX64.EFI) -> kernel (vmlinuz)
             -> initramfs (busybox rootfs) -> /init -> text shell
```

No root / no chroot / no pacman: everything is read from `/usr` and written to
the FAT32 drive as the normal user.

## Drive layout

```
<ESP>/
├── EFI/BOOT/BOOTX64.EFI    # Limine UEFI bootloader (standard UEFI path)
├── EFI/BOOT/limine.conf    # config (searched first on EFI)
├── limine.cfg              # ESP-root fallback copy (same content)
├── vmlinuz                 # linux-cachyos-lts 6.18.55-2 kernel
└── initramfs.cpio.gz       # ~1.7 MB busybox rootfs (rootfs/ tree)
```

Only the **single FAT32 ESP** is used. No Linux partition, no userdata
partition yet — the whole system lives in the initramfs (the read-only rootfs
model). A separate rw userdata partition lands with Phase 2.

## Build & deploy

```
./flashdrive/build.sh                # auto-detects /run/media/dio/BA88-0A7A
./flashdrive/build.sh /mount/point   # explicit
```

Environment overrides: `KERNEL_VERSION`, `BUSYBOX`, `LIMINE_EFI`.

The script is idempotent — rerun after editing `rootfs/init` or `limine.conf`.

## Boot test

1. Rebuild + deploy (above).
2. Unmount the drive cleanly (`udisks2` GUI or `sync` + unmount).
3. Reboot, enter UEFI setup, put the USB first in the boot order,
   **Secure Boot OFF**.
4. Expect: Limine menu (one entry, "NativeRa Prototype", 3 s timeout) →
   kernel scroll (quiet) → `NativeRa — Prototype Phase 1` banner → shell.
5. In the shell: `ls /sys/firmware/efi`, `dmesg | tail`, `uname -r`,
   `free -m`, then `reboot`.

## Troubleshooting

- **Firmware can't find the stick** — some UEFI firmwares only auto-try
  `/EFI/BOOT/BOOTX64.EFI` when the FAT volume is flagged as the ESP.
  The drive is a single-partition FAT32 volume, which UEFI treats as an ESP.
  If it's refused, make the partition type an ESP GPT entry
  (`gdisk`/`sgdisk` set type `EF00`) and retry.
- **Limine menu shows but no entry** — Limine couldn't find `limine.conf`.
  It looks on EFI in this order: `<EFI app path>/limine.conf`
  (= `/EFI/BOOT/limine.conf`), then `/EFI/BOOT/limine.conf`, then
  `/boot/limine/limine.conf`, `/boot/limine.conf`, `/limine/limine.conf`,
  `/limine.conf` (all on the boot volume). Both primary copies are deployed.
- **Kernel loads, then black screen / no console** — console is on
  `tty1`/efifb; press `Esc` to leave quiet mode or add `console=tty1`
  to the entry's `cmdline` in `limine.conf`.
- **Stuck at "Loading module"** — initramfs is not a gzip-newc cpio.
  Verify with: `gzip -dc initramfs.cpio.gz | cpio -t | head`.

## Phase 2 (next)

- X11/SDL2 + fonts + `nativera-launcher` (graphical home screen)
- controller: `hid`/`usbhid` (built into the kernel) + Bluetooth stack
  (`bluetooth.ko.zst` + `btusb`) + iwd for Wi-Fi
- i915/amdgpu DRM + firmware blobs (`/usr/lib/firmware/{intel,brcm,...}`)
- split into read-only squashfs rootfs + rw `userdata` ext4 partition
  (Batocera model), games on `/userdata/nativera`

## Limine config reference (v12)

`flashdrive/limine.conf` is written for Limine **12.x**: config file name is
`limine.conf` (not `limine.cfg`), protocol is `linux` (not `limine_linux`),
the initramfs is passed as `module_path`, and paths use the `boot():/...`
prefix. Verified against `/usr/share/doc/limine/CONFIG.md`.
