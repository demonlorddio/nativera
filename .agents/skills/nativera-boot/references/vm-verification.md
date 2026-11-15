# VM boot verification

Booting in a VM before touching hardware is correct and saves reboot cycles. But
a VM test can pass while the real machine fails, and in this project it did.

## The failure mode: the test measured the bug

The cmdline was `console=tty0 console=ttyS0`, so `/dev/console` was the serial
port. On a real machine that meant a black screen. In the VM, the serial port was
attached to a log file, so:

- init's output went to `ttyS0`
- the harness captured `ttyS0` into a log
- the log was full of banner, mount check and prompt
- the harness reported **verified**

The test was reading from the exact port that was swallowing the output. It could
not have failed. Every "verified in VM" claim made that way was worthless.

**Rule: the serial port must be detached to test what a user sees.**

## Two configurations, both required

| Config | Serial | Proves |
|---|---|---|
| `NO_SERIAL=1` | detached | the physical screen works - this is the real-machine case |
| default | attached to file | the serial log is readable, useful for kernel-level debugging |

A run with serial attached is a debugging aid. A run with serial detached is the
verification. Do not conflate them.

## Building the test image

The VM must boot the *same payload* as the stick, not a rebuild. Two ways, and
the second is the one that catches stale deploys:

```sh
# rebuild from source - tests the code
./flashdrive/vm-test.sh

# stage the media's own bytes, then boot those - tests the stick
cp <mount>/{vmlinuz,initramfs.cpio.gz,limine.conf} /tmp/nativera-vm/deploy/
cp <mount>/EFI/BOOT/{BOOTX64.EFI,limine.conf} /tmp/nativera-vm/deploy/EFI/BOOT/
SKIP_BUILD=1 ./flashdrive/vm-test.sh
```

A raw disk image with the same GPT + ESP layout is built with `parted` and
`mtools`, so no loop device and no root are needed:

```sh
truncate -s 256M disk.img
parted -s disk.img mklabel gpt
parted -s disk.img mkpart ESP fat32 1MiB 100%
parted -s disk.img set 1 esp on
mformat -i "disk.img@@1048576" -F -v NATIVERA ::
mcopy -i "disk.img@@1048576" vmlinuz ::/vmlinuz
```

The `@@1048576` offset addresses the partition inside the raw image.

## VirtualBox specifics

```sh
VBoxManage createvm --name Test --ostype Linux26_64 --register
VBoxManage modifyvm Test --memory 1024 --cpus 2 --firmware efi \
    --boot1 disk --boot2 none --uart1 0x3F8 4 --uartmode1 file serial.log
VBoxManage convertfromraw disk.img disk.vdi --format VDI
VBoxManage storageattach Test --storagectl SATA --port 0 --device 0 \
    --type hdd --medium disk.vdi
VBoxManage startvm Test --type headless
```

`--firmware efi` is required - the default BIOS will not exercise the UEFI path
at all, so a passing BIOS boot proves nothing about the stick.

Detach serial with `--uart1 off --uartmode1 disconnected`.

## Evidence to collect

1. **VM state.** `VBoxManage showvminfo Test --machinereadable`. `running` means
   it reached the shell. `poweroff` means init exited and powered off - which
   looks identical to a crash unless you check.
2. **A screenshot of the emulated screen.** `VBoxManage controlvm Test
   screenshotpng out.png`. This is the only artefact that shows what a user
   would see. Read the image, do not infer from logs.
3. **The serial log**, if attached, for kernel-level detail.

A run that reports "no serial output captured" but leaves the VM `running` with a
clean screen is a **pass**. That is exactly what a real machine does.

## Reading the screen

The screenshot must show:

- the banner
- `/proc: mounted`, `/sys: mounted`, `/dev: mounted`
- a `~ #` prompt

If the screen is black but the VM is running, output is going somewhere else -
check the cmdline console order. If the screen shows a prompt but no banner, init
failed before the banner and the prompt is from a fallback.

## What a VM cannot prove

| Not covered | Why |
|---|---|
| Secure Boot | VirtualBox EFI does not enforce it the way real firmware does |
| GPU/DRM bring-up | no real display controller; KMS paths differ |
| USB/Bluetooth enumeration | emulated devices are not real ones |
| Firmware boot-entry quirks | vendor firmware varies wildly |
| Timing-sensitive races | VM is slower and more forgiving |

So: VM pass is necessary, not sufficient. Real hardware is the final gate, and
the only thing that closes it is a person seeing the prompt.

## Honest reporting

If a check was not run, say **NOT ASSESSED**. Never write "verified" for
something inferred from a log that could be produced by the bug itself. The cost
of an assumed pass is a reboot cycle and a loss of trust in every other claim.

When reporting a VM result, state which configuration was used:

> Verified with the stick's own bytes, serial detached: banner rendered, /proc
> /sys /dev mounted, 133 dev entries, `~ #` prompt on the physical screen.

That sentence is falsifiable. "Verified in VM" is not.
