# Kernel cmdline and console routing

The cmdline is the highest-leverage line in the whole boot. It is also where the
most expensive bug in this project lived.

## `/dev/console` is the LAST `console=`

This is the rule that caused a black screen after a healthy boot.

```
cmdline: loglevel=7 rdinit=/init console=tty0 console=ttyS0
```

`/dev/console` resolves to `ttyS0` here, because `ttyS0` is last. Consequences:

- The **kernel** writes its own log to every registered console, so the log
  appears on the screen. The boot looks like it is progressing.
- **init** writes to `/dev/console`, which is `ttyS0`. Nothing is plugged into
  `ttyS0`. The banner, the mount check and the shell prompt all go nowhere.
- The screen goes black at exactly the moment the kernel hands off to init.

The machine booted correctly. Nobody could see it. There is no error, no hang,
no message - just a black screen that looks identical to a hard failure.

Correct ordering:

```
cmdline: loglevel=7 rdinit=/init console=ttyS0 console=tty0
```

`tty0` last means `/dev/console` is the physical screen. `ttyS0` first still
mirrors the kernel log to serial, which is what makes VM testing possible.

## Diagnostic: which console is which

Inside a running shell:

```sh
tr ' ' '\n' < /proc/cmdline | grep '^console='
cut -d' ' -f7 /proc/self/stat      # tty_nr: 0 means NO controlling tty
tty                                 # the device name, or "not a tty"
```

`tty_nr` is the reliable signal. A non-zero value means a controlling terminal
exists. `tty` alone can print a name for a tty that is not controlling.

## `[ -t 1 ]` cannot detect a dead serial console

A serial port with nothing attached is still a valid tty. `[ -t 1 ]` returns
true. So you cannot detect "output is going nowhere" with an isatty check - you
have to parse the cmdline:

```sh
LAST_CONSOLE="$(tr ' ' '\n' < /proc/cmdline | grep '^console=' | tail -1)"
case "$LAST_CONSOLE" in
    console=ttyS*|console=ttyUSB*)
        exec >/dev/tty0 2>&1 </dev/tty0 ;;
esac
```

This is a safety net, not a substitute for correct ordering. Keep the cmdline
right and the net never fires.

## Useful cmdline parameters

| Parameter | Effect | When |
|---|---|---|
| `rdinit=/init` | PID 1 from the initramfs | always, for an initramfs-root boot |
| `init=/sbin/init` | PID 1 from the real root | once a root filesystem exists |
| `loglevel=7` | full kernel log to console | bring-up. Never ship `quiet` while debugging |
| `quiet` | suppress most kernel log | only after the chain is proven |
| `console=tty0` | physical VT | must be last when combined with serial |
| `console=ttyS0` | serial | put first |
| `earlyprintk=efi` | console before the normal console init | when nothing appears at all |
| `nomodeset` | skip KMS driver | when a GPU driver blacks the screen after handoff |

## Bring-up vs shipped

During bring-up, verbose is correct: `loglevel=7`, no `quiet`. You need to see
failures. A `quiet` cmdline turns every bug into a black screen with no evidence,
which is the single most expensive debugging mode there is.

Once the chain is proven on real hardware, `quiet loglevel=3` is reasonable for
a console distro. Change it in a separate commit so a regression is attributable.

## Symptom table

| What you see | Meaning |
|---|---|
| Logs scroll, then black | `/dev/console` on a serial port - reorder |
| Nothing at all, no log | wrong kernel path, or `earlyprintk` needed |
| Logs, then a hang | init blocked - usually a mount waiting on a missing device |
| Logs, then `Kernel panic - not syncing: Attempted to kill init` | init exited or could not exec |
| Logs, then reboot loop | init powered off, or PID 1 crashed |
| Prompt appears but typing does nothing | tty is not the one you are looking at |
