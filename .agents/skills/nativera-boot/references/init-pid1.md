# init as PID 1

The init script is PID 1. That gives it two properties that cause most of the
confusing behaviour:

1. It is a **session leader with no controlling terminal**.
2. When it exits, the kernel panics.

## Why `exec cttyhack /bin/sh` fails

The obvious way to drop to a shell:

```sh
exec cttyhack /bin/sh
```

Result:

```
/bin/sh: can't access tty; job control turned off
```

The prompt appears, so it looks like a cosmetic warning. It is not. There is no
Ctrl-C, no `fg`, no `bg`, no `$?` from a background job, and interactive tools
misbehave. For a console distro that is a real defect.

**Why:** `cttyhack` calls `setsid()` to put the shell in a new session, then opens
the console to acquire it as the controlling terminal. But PID 1 is *already* a
session leader, and `setsid()` on a session leader fails with `EPERM`. The shell
keeps its file descriptors (fd 0 and 1 are `/dev/console`, and it *is* a tty) but
never gets a **controlling** terminal. `tty_nr` in `/proc/self/stat` stays 0.

This is why `[ -t 0 ]` returns true and yet job control is off. Two different
questions: "is this fd a tty" and "is this my controlling tty".

## The fix

busybox documents the pattern for exactly this case:

```sh
setsid cttyhack /bin/sh
```

`setsid` forks into a fresh session (the child is not a session leader yet, so
its `setsid()` succeeds), then `cttyhack` acquires the console as that session's
controlling terminal.

Verified: `tty_nr=1088`, `tty=/dev/ttyS0`, `set -m` succeeds, background jobs
work.

Do not `exec` it - let it run as a child and keep PID 1 alive to reap the exit:

```sh
setsid cttyhack /bin/sh

echo "Shell exited. Powering off."
sync
exec poweroff -f
```

## Verifying job control

Inside the shell:

```sh
cut -d' ' -f7 /proc/self/stat     # non-zero = controlling tty exists
tty                                # /dev/tty0 or /dev/ttyS0
set -m && echo "job control OK"
sleep 0.2 & wait && echo "bg job OK"
```

All four must pass. The absence of the warning message alone is not proof - the
warning is just a printf, it can be suppressed while the underlying condition
remains.

## Console fallback

Because `/dev/console` follows the last `console=` on the cmdline, init should
defend itself against a cmdline that points at a dead serial port:

```sh
LAST_CONSOLE="$(tr ' ' '\n' < /proc/cmdline 2>/dev/null | grep '^console=' | tail -1)"
case "$LAST_CONSOLE" in
    console=ttyS*|console=ttyUSB*)
        if [ -c /dev/tty0 ]; then
            exec >/dev/tty0 2>&1 </dev/tty0
        fi
        ;;
esac
```

Note this must run *after* `/dev` is mounted (devtmpfs provides `/dev/tty0`) and
*after* `/proc` is mounted (the cmdline is read from `/proc/cmdline`).

## init checklist

- [ ] `PATH` set, `TERM=linux`, `HOME=/`
- [ ] `mkdir -p /proc /sys /dev /run` before any mount
- [ ] `mount -t proc`, `-t sysfs`, `-t devtmpfs` each reported on failure
- [ ] `/dev/console`, `/dev/null`, `/dev/tty` created if devtmpfs did not
- [ ] console fallback if last `console=` is serial
- [ ] banner printed **after** mounts, never `clear` before it
- [ ] diagnostics printed after the banner so nothing erases them
- [ ] `setsid cttyhack /bin/sh`, not `exec cttyhack`
- [ ] clean `poweroff -f` after the shell exits

## Do not `clear` during bring-up

`clear` wipes the scrollback, which is exactly the evidence needed to debug a
failed boot. The banner in this project deliberately does not clear. Keep it that
way until the chain is proven on real hardware.

## Exit behaviour

If init exits, the kernel panics with `Attempted to kill init`. That is why the
script ends with `exec poweroff -f` rather than falling off the end - a clean
shutdown instead of a panic, which matters when the next step is a reboot into
the next test.
