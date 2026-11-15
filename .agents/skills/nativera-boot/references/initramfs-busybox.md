# Initramfs and busybox

The initramfs is a cpio archive the kernel unpacks into a tmpfs and then execs
`/init` from. In NativeRa it is the entire root filesystem: busybox, three glibc
objects, an init script, and empty mountpoint directories.

## The stripped-busybox trap

`/usr/lib/initcpio/busybox` on Arch-family hosts is **not** a general busybox. It
is the mkinitcpio build, stripped to 147 applets, and it is missing `mount`,
`date`, `tr` and `find`.

Using it produces this, at runtime, not at build time:

```
mount: applet not found
```

Every `mount` in init fails. `/proc`, `/sys` and `/dev` never mount. The shell
comes up with no `/proc`, so `uname`, `free` and `ps` misbehave, and with no
`/dev` there is no controlling tty. It looks like five separate bugs. It is one.

**Fix:** use a full static busybox. This project vendors one at
`flashdrive/vendor/busybox-static` (1.36.1, 392 applets, statically linked).
`build.sh` prefers it and refuses to build if required applets are missing.

To get one on an Arch-family host without installing anything:

```sh
pacman -Sp busybox                      # prints the package URL
curl -sSL -o /tmp/busybox.pkg.tar.zst "<that url>"
mkdir -p /tmp/bb && tar --zstd -xf /tmp/busybox.pkg.tar.zst -C /tmp/bb
file /tmp/bb/usr/bin/busybox            # want: statically linked
```

A static busybox needs no glibc in the archive at all. A dynamic one needs
`ld-linux-x86-64.so.2`, `libc.so.6` and whatever else `ldd` reports.

## The applet check that lies

This check is wrong and it rejected a perfectly good 392-applet busybox:

```sh
# BROKEN
for a in mount date tr; do
    "$BUSYBOX" --list | grep -qx "$a" || MISSING="$MISSING $a"
done
```

`grep -q` exits on the first match. busybox then writes to a closed pipe, takes
SIGPIPE, and `set -o pipefail` turns that into a non-zero status. Applets near
the top of the list (`echo` is at line 71 of 392) report as missing.

Correct - capture the list once:

```sh
APPLETS="$("$BUSYBOX" --list 2>/dev/null)"
for a in mount date tr; do
    printf '%s\n' "$APPLETS" | grep -qx -- "$a" || MISSING="$MISSING $a"
done
```

## Applet list must match what init calls

A missing applet fails at **runtime** with `not found`, not at build time. The
symlink list in `build.sh` and the commands in `rootfs/init` have to agree. Keep
the build-time check as the enforcement, and add to it whenever init grows a new
command.

Minimum set for this init:

```
sh mount umount mknod mkdir cat echo ls cp rm ln sleep dmesg head tail
cut wc date env chmod chown free clear kill ps pwd mv rmdir uname awk
sed sort tr find printf test true false tee sync cttyhack setsid
```

`setsid` is easy to forget and is required for the PID 1 shell (see
`init-pid1.md`).

## Mountpoints must exist inside the archive

`mount -t proc none /proc` fails if `/proc` is not already a directory in the
unpacked tree. The kernel does not create these. `build.sh` must `mkdir -p` them
before packing:

```
proc sys dev run
```

Symptom when missing: mounts fail silently, `/proc` is empty, `/dev` has no
console. Same surface as the stripped-busybox bug, different cause - check the
archive contents before blaming the binary.

## Packaging: newc + gzip, root-owned

```sh
( cd "$ROOT" && \
  find . -print0 | cpio -o -0 -R 0:0 --format=newc 2>/dev/null | \
      gzip -9 > initramfs.cpio.gz )
```

- `--format=newc` is what the kernel expects. `odc` and `bin` do not work.
- `-R 0:0` forces root ownership. Files owned by a normal user can fail to
  unpack with the right permissions.
- `-0` with `find -print0` handles filenames with spaces.

Verify the archive independently:

```sh
gzip -dc initramfs.cpio.gz | cpio -t | head
mkdir -p /tmp/ir && cd /tmp/ir && gzip -dc <path> | cpio -idm --quiet
./bin/busybox --list | wc -l          # expect ~392, not ~147
```

## What belongs in the initramfs

Only what is needed **before** the real root mounts. Once Phase 2 has a squashfs
root, the initramfs shrinks back to: busybox, the modules needed to find and
mount the root, and a short init. DRM, Bluetooth, firmware blobs and the
front-end all belong in the root filesystem, not here.

Adding a module to the initramfs means adding it to the archive *and* ensuring
its dependencies are present. `modprobe` needs `/lib/modules/<ver>/modules.dep`,
which means shipping `depmod` output too. Prefer building the driver into the
kernel over shipping it as a module in the initramfs.
