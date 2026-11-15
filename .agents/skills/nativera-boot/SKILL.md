---
name: nativera-boot
description: Build, debug and verify NativeRa's bootable media - UEFI/ESP layout, Limine config, kernel cmdline, busybox initramfs, PID 1 init, and VM boot verification. Use when a NativeRa stick or image boots to a black screen, drops to a shell with the wrong console, fails to appear in firmware, hits "applet not found", or before deploying any change to flashdrive/. Also use when assembling a new initramfs, adding a kernel module, or moving from initramfs to a squashfs root.
surfaces:
  cli: true
  gui: true
triggers:
  - nativeRa boot
  - flashdrive
  - initramfs
  - busybox applet
  - limine
  - black screen on boot
  - can't access tty
  - ESP partition
  - UEFI boot
---

# NativeRa Boot

Boot work on NativeRa is a chain of five links, and every link fails *silently*:

```
UEFI firmware -> Limine (BOOTX64.EFI) -> kernel (vmlinuz)
              -> initramfs (busybox rootfs) -> /init -> shell
```

There is no systemd, no journal, no rescue shell, no error dialog. A broken link
produces a black screen, a hang, or a prompt that cannot be typed into. That is
the whole failure vocabulary. This skill exists because "it looks like it booted"
is not evidence, and twice in this project a boot was declared verified when the
test itself was hiding the fault.

## The one rule

**A boot is verified only when you have seen the shell prompt on the physical
console, with the serial port detached.**

Not a serial log. Not a screenshot with a serial port attached. Not a clean exit
code. The prompt, on the screen, with nothing else wired up.

Why this is absolute: `/dev/console` follows the *last* `console=` on the kernel
cmdline. If that is a serial port, init's output goes there and the screen stays
black while the boot is perfectly healthy. A VM test that captures the serial log
is reading from the exact port that is swallowing the output, so it reports
success no matter what the screen does. This has already happened once here.

## Verification protocol

Run in this order. Do not skip to the end.

| # | Check | Command | Pass |
|---|---|---|---|
| 1 | Payload builds | `./flashdrive/build.sh /tmp/deploy` | exits 0, all 5 files listed |
| 2 | Initramfs contents | `scripts/verify-initramfs.sh <initramfs.cpio.gz>` | applets + mountpoints + init OK |
| 3 | Boot with serial detached | `NO_SERIAL=1 ./flashdrive/vm-test.sh` | VM state `running`, screen shows `~ #` |
| 4 | Boot with serial attached | `./flashdrive/vm-test.sh` | same, plus serial log readable |
| 5 | Deploy to media | `./flashdrive/build.sh <mount>` | verify step prints all 5 sizes |
| 6 | Media bytes match source | `scripts/verify-esp.sh <mount>` | every md5 matches |
| 7 | Boot the media's own bytes | stage from media, `NO_SERIAL=1` | screen shows `~ #` |

Step 7 matters: steps 1-4 test a *rebuild*. Step 7 tests what is actually on the
stick. A stale deploy passes every earlier step and fails at the hardware.

If a check cannot be run, record it as **NOT ASSESSED**. Never infer a pass. A
missing check is a known gap; an assumed pass is a lie that costs a reboot cycle.

## Symptom -> cause routing

Open the reference only for the symptom you have.

| Symptom | Most likely cause | Reference |
|---|---|---|
| Logs scroll, then black screen | `/dev/console` on a serial port | `references/cmdline-and-console.md` |
| `mount: applet not found` | stripped busybox baked in | `references/initramfs-busybox.md` |
| `can't access tty; job control turned off` | cttyhack as PID 1 | `references/init-pid1.md` |
| `/proc` or `/dev` empty after boot | mountpoints absent from cpio | `references/initramfs-busybox.md` |
| Firmware shows no bootable device | partition not typed EF00 | `references/esp-uefi.md` |
| Limine: "no valid entries" | 5.x `:` syntax in a 12.x config | `references/esp-uefi.md` |
| Kernel loads, no console at all | framebuffer or console= wrong | `references/cmdline-and-console.md` |
| VM powers off by itself | shell exited, init powered off | `references/vm-verification.md` |
| Boots in VM, not on hardware | VM hid it (serial, EFI, timing) | `references/vm-verification.md` |

## Hard-won invariants

These are all bugs that actually shipped in this project. Each one looked fine
until it was booted.

1. **`console=tty0` must be LAST.** `/dev/console` is the last `console=`. Put
   `console=ttyS0` first if you want serial mirroring.
2. **Never use `/usr/lib/initcpio/busybox`.** It is a stripped 147-applet build
   with no `mount`, `date`, `tr` or `find`. Use `flashdrive/vendor/busybox-static`.
3. **`exec cttyhack /bin/sh` cannot work as PID 1.** PID 1 is already a session
   leader, so `setsid()` fails with EPERM. Use `setsid cttyhack /bin/sh`.
4. **Mountpoints must exist in the cpio.** `mount -t proc none /proc` fails if
   `/proc` is not a directory in the archive. `build.sh` creates them; verify.
5. **A build-time applet check must not pipe into `grep -q`.** grep exits early,
   busybox takes SIGPIPE, `pipefail` reports a false missing applet. Capture the
   list once into a variable.
6. **Verify the deploy landed.** A partial copy looks like a working stick and
   boots to nothing. Check every file is non-empty after writing.
7. **The ESP must be GPT type EF00.** UEFI firmware only creates a boot entry for
   a properly typed EFI System partition.

## Procedure contract

Every change to boot media follows this shape, borrowed from server-operations
discipline because it fits: name the target, state preconditions, make a bounded
change, keep a rollback route, verify with evidence.

- **Target**: which file, which partition, which device. Never "the stick".
- **Preconditions**: is it mounted, is it the right device, is it USB.
- **Bounded change**: one link of the chain at a time.
- **Rollback**: git. Every boot change is committed before it is deployed.
- **Verification**: the protocol above, with the actual output quoted.

Destructive operations (repartitioning, formatting) must refuse anything that is
not removable media, and must require the device path to be typed back.

## Phase 2 direction

The current model is a single FAT32 ESP with everything in the initramfs. That
does not scale: a DRM stack, firmware blobs and a Bluetooth stack will not fit
comfortably, and nothing is writable. Phase 2 moves to the Batocera three-tier
model - FAT boot partition, read-only squashfs root, writable userdata - which
is why `references/esp-uefi.md` covers partition layout in detail rather than
just the current single-partition case.

When adding modules to the initramfs, the question to answer first is whether the
module is needed *before* the root filesystem mounts. If not, it belongs in the
root, not the initramfs.
