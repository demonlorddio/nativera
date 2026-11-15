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
├── limine.conf             # ESP-root fallback copy (same content)
├── vmlinuz                 # linux-cachyos-lts 6.18.60-2 kernel (auto-detected)
└── initramfs.cpio.gz       # ~2.1 MB busybox rootfs (rootfs/ tree)
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

### busybox

`build.sh` uses `flashdrive/vendor/busybox-static` (a full static busybox
1.36.1, 392 applets). **Do not** point it at `/usr/lib/initcpio/busybox`: that
is a stripped mkinitcpio build with 147 applets and no `mount`, `date`, `tr`
or `find`. Every mount in `init` then fails with `applet not found`, which
leaves the shell with no `/proc` and no controlling tty. `build.sh` verifies
the required applets at build time and fails loudly if any are missing.

## Boot test (VM, before touching hardware)

```
./flashdrive/vm-test.sh          # build, boot in VirtualBox, print serial log
./flashdrive/vm-test.sh --keep   # leave the VM registered to inspect
```

This writes the same payload to a raw GPT disk image with an identical ESP
layout, boots it under EFI firmware, and captures the full serial console to
`/tmp/nativera-vm/serial.log`. Use it after every change to `init`,
`limine.conf` or `build.sh` — it takes ~25 s and needs no reboot.

## Boot test (real hardware)

1. Rebuild + deploy (above).
2. Unmount the drive cleanly (`udisks2` GUI or `sync` + unmount).
3. Reboot, enter UEFI setup, put the USB first in the boot order,
   **Secure Boot OFF**.
4. Expect: Limine menu (one entry, "NativeRa Prototype", 3 s timeout) →
   kernel log (verbose, `loglevel=7`) → `NativeRa — Prototype Phase 1`
   banner → mount check → `~ #` prompt.
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
- **`can't access tty; job control turned off`** — `init` launched the shell
  with `exec cttyhack /bin/sh`. PID 1 is already a session leader, so
  cttyhack's `setsid()` fails with EPERM and the shell never gets a
  *controlling* tty. The fix is `setsid cttyhack /bin/sh`, which is the
  pattern busybox documents for this exact case. Verify with
  `cut -d' ' -f7 /proc/self/stat` inside the shell: non-zero means a
  controlling tty.
- **`mount: applet not found`** — the wrong busybox got baked in. See the
  busybox section above.
- **Kernel loads, then black screen / no console** — console is on
  `tty0`/efifb. The entry's `cmdline` sets `console=tty0 console=ttyS0`;
  add `console=tty1` if the firmware only lights up a VT.
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
