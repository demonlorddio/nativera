#!/bin/bash
# NativeRa - reproducible flash-drive build pipeline (Phase 1)
#
# Usage:
#   ./flashdrive/build.sh                 # auto-detects the drive mount
#   ./flashdrive/build.sh /mount/point    # explicit drive mount point
#
# What it does:
#   1. assembles a minimal busybox rootfs (bin/lib/etc + /init)
#   2. packages it as initramfs.cpio.gz (newc + gzip)
#   3. writes the UEFI layout to the drive:
#        EFI/BOOT/BOOTX64.EFI    (Limine UEFI)
#        EFI/BOOT/limine.conf    (searched first on EFI)
#        limine.conf             (ESP-root fallback candidate)
#        vmlinuz                 (kernel)
#        initramfs.cpio.gz
#
# No root required: everything is read from /usr and written to the
# user-mounted FAT32 drive.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/flashdrive"
OUT="$REPO/flashdrive/build"

# --- inputs (override via environment) --------------------------------------
# NOTE: the host kernel is NOT what we boot; we bundle a kernel from /usr/lib/modules.
# Auto-detect so a host kernel update can never silently break the build:
# prefer the newest *-lts module dir, else the newest module dir that has a vmlinuz.
if [ -z "${KERNEL_VERSION:-}" ]; then
    KERNEL_VERSION="$(for d in /usr/lib/modules/*-lts; do
                          [ -f "$d/vmlinuz" ] && basename "$d"
                      done | sort -V | tail -1)"
fi
if [ -z "${KERNEL_VERSION:-}" ]; then
    KERNEL_VERSION="$(for d in /usr/lib/modules/*; do
                          [ -f "$d/vmlinuz" ] && basename "$d"
                      done | sort -V | tail -1)"
fi
KMOD="/usr/lib/modules/$KERNEL_VERSION"
echo "==> kernel: ${KERNEL_VERSION:-<none found>}"

# BUSYBOX: do NOT default to /usr/lib/initcpio/busybox. That binary is a
# stripped mkinitcpio build with only 147 applets and NO mount, date, tr or
# find. Using it makes every mount in rootfs/init fail with
# "mount: applet not found", which silently leaves the shell with no /proc and
# no controlling tty. Prefer a full static busybox; verify the applets we need.
if [ -z "${BUSYBOX:-}" ]; then
    for cand in "$SRC/vendor/busybox-static" /tmp/busybox-static \
                /usr/local/bin/busybox-static /usr/bin/busybox /bin/busybox; do
        [ -x "$cand" ] && BUSYBOX="$cand" && break
    done
fi
BUSYBOX="${BUSYBOX:-/usr/lib/initcpio/busybox}"
LIMINE_EFI="${LIMINE_EFI:-/usr/share/limine/BOOTX64.EFI}"
DRIVE="${1:-/run/media/$(id -un)/BA88-0A7A}"

# --- sanity checks -----------------------------------------------------------
[ -x "$BUSYBOX" ]           || { echo "missing busybox: $BUSYBOX" >&2; exit 1; }
[ -f "$KMOD/vmlinuz" ]     || { echo "missing kernel: $KMOD/vmlinuz" >&2; exit 1; }
[ -f "$LIMINE_EFI" ]       || { echo "missing Limine UEFI: $LIMINE_EFI" >&2; exit 1; }
[ -d "$DRIVE" ] && [ -w "$DRIVE" ] ||
    { echo "drive not mounted/writable: $DRIVE (is it plugged in & mounted?)" >&2; exit 1; }

# Fail at BUILD time, not boot time, if the busybox cannot do what init needs.
# NOTE: capture the applet list ONCE into a variable. Piping `busybox --list`
# into `grep -q` makes grep exit on first match, busybox take SIGPIPE, and
# `set -o pipefail` then report a false failure for applets near the top of
# the list. That bug previously rejected a perfectly good 392-applet busybox.
echo "==> busybox: $BUSYBOX"
APPLETS="$("$BUSYBOX" --list 2>/dev/null)"
MISSING=""
for a in sh mount umount mknod mkdir cat echo ls cp rm ln sleep dmesg head tail \
         cut wc date env chmod chown free clear kill ps pwd mv rmdir uname awk \
         sed sort tr find printf test true false tee sync cttyhack; do
    printf '%s\n' "$APPLETS" | grep -qx -- "$a" || MISSING="$MISSING $a"
done
if [ -n "$MISSING" ]; then
    echo "BUSYBOX IS INCOMPLETE - missing applets:$MISSING" >&2
    echo "Point BUSYBOX= at a full static busybox (e.g. busybox.net 1.35.0 musl)." >&2
    exit 1
fi
echo "==> busybox applets: all required present"

echo "==> build dir: $OUT"
rm -rf "$OUT"; mkdir -p "$OUT"

# --- 1. rootfs tree -----------------------------------------------------------
ROOT="$OUT/rootfs"
mkdir -p "$ROOT"/bin "$ROOT"/sbin "$ROOT"/etc "$ROOT"/root "$ROOT"/home \
         "$ROOT"/tmp "$ROOT"/var "$ROOT"/usr/bin "$ROOT"/usr/sbin \
         "$ROOT"/usr/lib "$ROOT"/usr/lib64 \
         "$ROOT"/proc "$ROOT"/sys "$ROOT"/dev "$ROOT"/run
# /proc /sys /dev /run MUST exist as mountpoints before init runs. Without them
# every mount in rootfs/init fails silently and the shell comes up with no
# /proc (empty uptime/mem) and no controlling tty.

# busybox + the three glibc pieces its dynamic loader needs
cp "$BUSYBOX"                        "$ROOT/bin/busybox"
cp /usr/lib64/ld-linux-x86-64.so.2  "$ROOT/usr/lib64/"
cp /usr/lib/libc.so.6               "$ROOT/usr/lib/"
cp /usr/lib/libcrypt.so.2           "$ROOT/usr/lib/"
ln -s usr/lib   "$ROOT/lib"
ln -s usr/lib64 "$ROOT/lib64"

# applet symlinks
# NOTE: keep this list in sync with what rootfs/init actually calls. A missing
# applet fails at runtime with "not found", not at build time.
cd "$ROOT/bin"
for a in sh mount umount ls cat echo sleep mkdir modprobe insmod delmod lsmod \
         ip ifconfig reboot poweroff halt grep cp rm ln touch dmesg head tail \
         more tr cut find wc date env chmod chown mknod free clear kill ps \
         pwd mv rmdir uname awk sed sort uniq xargs basename dirname printf \
         test true false tee sync cttyhack; do
    ln -sf busybox "$a"
done
cd "$ROOT/sbin"
for a in insmod modprobe reboot poweroff halt; do
    ln -sf ../bin/busybox "$a"
done

install -m 755 "$SRC/rootfs/init" "$ROOT/init"
printf 'nativera\n' > "$ROOT/etc/hostname"

# --- 1b. networking (optional, NET=1) ----------------------------------------
# The kernel ships EVERY ethernet driver as a module (CONFIG_E1000E=m,
# CONFIG_R8169=m, ...) and this initramfs has no /lib/modules, so without this
# block the boot has no network interface at all and nothing can load one.
#
# NET=1 adds: the common NIC modules + their firmware, libz (dropbear needs it),
# dropbear itself, and a host key generated at build time.
if [ "${NET:-0}" = "1" ]; then
    echo "==> networking: enabled"
    KVER="$KERNEL_VERSION"
    MODROOT="$ROOT/lib/modules/$KVER"
    mkdir -p "$MODROOT"

    # Copy a module and everything modules.dep says it needs.
    # A missing module is a HARD ERROR, not a skip. Silently skipping is how a
    # wrong path (drivers/net/phy/realtek.ko.zst instead of
    # drivers/net/phy/realtek/realtek.ko.zst) shipped an image with no PHY
    # driver, which produced an all-zero MAC at boot and no DHCP.
    MOD_FAIL=0
    copy_mod() {
        local rel="$1"
        local src="/usr/lib/modules/$KVER/$rel"
        if [ ! -f "$src" ]; then
            echo "    MISSING MODULE: $rel" >&2
            echo "      no such file: $src" >&2
            MOD_FAIL=1
            return 0
        fi
        mkdir -p "$MODROOT/$(dirname "$rel")"
        # Decompress .ko.zst -> .ko. The kernel supports compressed modules
        # (CONFIG_MODULE_DECOMPRESS=y) but busybox insmod/modprobe does NOT pass
        # the MODULE_INIT_COMPRESSED_FILE flag, so a .ko.zst fails at load with
        # "Invalid ELF header magic: != ELF". Ship plain .ko files.
        case "$rel" in
            *.ko.zst)
                unzstd -q -c "$src" > "$MODROOT/${rel%.zst}" ;;
            *)
                cp "$src" "$MODROOT/$rel" ;;
        esac
        # dependencies, from modules.dep (paths are relative to /usr/lib/modules/<ver>)
        awk -v m="$rel" 'index($0, m ":")==1 { sub(/^[^:]*: */,""); print }' \
            "/usr/lib/modules/$KVER/modules.dep" 2>/dev/null | tr ' ' '\n' | \
        while read -r dep; do
            [ -n "$dep" ] || continue
            [ -f "/usr/lib/modules/$KVER/$dep" ] || continue
            local dst="$MODROOT/${dep%.zst}"
            [ -f "$dst" ] && continue
            mkdir -p "$MODROOT/$(dirname "$dep")"
            case "$dep" in
                *.ko.zst) unzstd -q -c "/usr/lib/modules/$KVER/$dep" > "$dst" ;;
                *)        cp -n "/usr/lib/modules/$KVER/$dep" "$MODROOT/$dep" 2>/dev/null || true ;;
            esac
        done
    }

    for m in kernel/drivers/net/ethernet/intel/e1000e/e1000e.ko.zst \
             kernel/drivers/net/ethernet/intel/e1000/e1000.ko.zst \
             kernel/drivers/net/ethernet/intel/igb/igb.ko.zst \
             kernel/drivers/net/ethernet/intel/igc/igc.ko.zst \
             kernel/drivers/net/ethernet/realtek/r8169.ko.zst \
             kernel/drivers/net/phy/realtek/realtek.ko.zst \
             kernel/drivers/net/phy/mdio_devres.ko.zst \
             kernel/drivers/net/mii.ko.zst; do
        copy_mod "$m"
    done

    # PHY drivers are attached at RUNTIME by PHY ID, not via modules.dep, so
    # they never show up as a dependency of the MAC driver. r8169's own deps
    # list only mdio/libphy/mdio-bus - the Realtek PHY is found by probing the
    # MDIO bus. Without it the link never comes up and the MAC reads as
    # 00:00:00:00:00:00. Ship the common ones explicitly.
    for p in kernel/drivers/net/phy/realtek/realtek.ko.zst \
             kernel/drivers/net/phy/micrel.ko.zst \
             kernel/drivers/net/phy/broadcom.ko.zst \
             kernel/drivers/net/phy/marvell.ko.zst \
             kernel/drivers/net/phy/bcm-phy-lib.ko.zst; do
        [ -f "/usr/lib/modules/$KVER/$p" ] && copy_mod "$p"
    done

    if [ "$MOD_FAIL" -ne 0 ]; then
        echo "NETWORKING BUILD FAILED: required module(s) missing (see above)" >&2
        exit 1
    fi

    # modules.dep for the subset we shipped, so modprobe resolves dependencies.
    if command -v depmod >/dev/null 2>&1; then
        depmod -b "$ROOT" "$KVER" 2>/dev/null || true
    fi

    # Realtek PHY firmware: r8169 loads one of these at probe time and fails
    # without it on most RTL8111/8125 parts.
    if [ -d /usr/lib/firmware/rtl_nic ]; then
        mkdir -p "$ROOT/usr/lib/firmware/rtl_nic"
        cp /usr/lib/firmware/rtl_nic/*.zst "$ROOT/usr/lib/firmware/rtl_nic/" 2>/dev/null || true
    fi

    # libz: dropbear links against it, the base rootfs does not carry it.
    cp -L /usr/lib/libz.so.1 "$ROOT/usr/lib/libz.so.1"

    # dropbear + a host key generated now, so the image is self-contained.
    if [ -x "$SRC/vendor/dropbear/dropbear" ]; then
        install -m 755 "$SRC/vendor/dropbear/dropbear"    "$ROOT/usr/sbin/dropbear"
        install -m 755 "$SRC/vendor/dropbear/dropbearkey" "$ROOT/usr/sbin/dropbearkey"
        mkdir -p "$ROOT/etc/dropbear"
        "$SRC/vendor/dropbear/dropbearkey" -t ed25519 \
            -f "$ROOT/etc/dropbear/dropbear_ed25519_host_key" >/dev/null 2>&1
        chmod 600 "$ROOT/etc/dropbear/dropbear_ed25519_host_key"
        echo "    dropbear + host key installed"
    else
        echo "    WARNING: vendor/dropbear missing, no ssh server" >&2
    fi

    # SSH authorized_keys: baked in from authorized_keys.pub at the repo root if
    # present, so a fresh stick is immediately reachable. Root has no password,
    # so key auth is the only way in.
    mkdir -p "$ROOT/root/.ssh"
    chmod 700 "$ROOT/root/.ssh"
    if [ -f "$REPO/authorized_keys.pub" ]; then
        install -m 600 "$REPO/authorized_keys.pub" "$ROOT/root/.ssh/authorized_keys"
        echo "    ssh key: authorized_keys.pub baked in"
    else
        echo "    ssh key: none (create authorized_keys.pub to enable key login)"
    fi

    # /etc/passwd + shadow: dropbear refuses to start without a root entry.
    printf 'root:x:0:0:root:/root:/bin/sh\n' > "$ROOT/etc/passwd"
    # EMPTY password field, not '*'. A '*' marks the account as LOCKED and
    # dropbear refuses it even for publickey auth, failing with
    # "Permission denied (publickey,password)". Empty = no password set, the
    # account is usable, and dropbear runs with -w so password auth is off.
    printf 'root::19000:0:99999:7:::\n'  > "$ROOT/etc/shadow"
    chmod 600 "$ROOT/etc/shadow"
    printf 'root:x:0:\n' > "$ROOT/etc/group"

    # /etc/shells: dropbear validates the login shell from /etc/passwd against
    # this file and REJECTS the login if it is not listed. With no /etc/shells
    # at all, every login fails with "root login rejected" and "0 fails" in the
    # log - the key is never even checked. This is a required file, not optional.
    printf '/bin/sh\n/bin/ash\n/bin/bash\n' > "$ROOT/etc/shells"

    # udhcpc event script. Without this udhcpc obtains a lease and applies
    # nothing (no address, no route, no resolv.conf) and exits 0 - which looks
    # like success. See rootfs/udhcpc-default.script.
    mkdir -p "$ROOT/usr/share/udhcpc"
    install -m 755 "$SRC/rootfs/udhcpc-default.script"         "$ROOT/usr/share/udhcpc/default.script"

    # Extra applets the network path needs.
    cd "$ROOT/bin"
    for a in udhcpc nc ping route netstat wget arping; do
        ln -sf busybox "$a"
    done
    cd "$OLDPWD"

    echo "    modules: $(find "$MODROOT" -name '*.ko' | wc -l) (.ko, decompressed)"
fi

# --- 1c. wifi (optional, WIFI=1) ---------------------------------------------
# Adds the 802.11 stack, the Intel and Realtek PCI/USB wifi drivers, their
# firmware, and iwd as the supplicant.
#
# iwd, not wpa_supplicant: iwd is 998 KB with one dependency (libell, 649 KB).
# wpa_supplicant is 5.6 MB and pulls libssl + libcrypto + libnl + libdbus +
# libpcsclite, about 15 MB total. For an initramfs that difference is decisive.
#
# Firmware is the real cost, not the drivers: iwlwifi alone is 203 files. Only
# the vendor trees for the drivers we actually ship are copied.
if [ "${WIFI:-0}" = "1" ]; then
    echo "==> wifi: enabled"
    KVER="$KERNEL_VERSION"
    MODROOT="$ROOT/lib/modules/$KVER"
    mkdir -p "$MODROOT"

    MOD_FAIL=0
    copy_mod() {
        local rel="$1"
        local src="/usr/lib/modules/$KVER/$rel"
        if [ ! -f "$src" ]; then
            echo "    MISSING MODULE: $rel" >&2
            MOD_FAIL=1
            return 0
        fi
        mkdir -p "$MODROOT/$(dirname "$rel")"
        case "$rel" in
            *.ko.zst) unzstd -q -c "$src" > "$MODROOT/${rel%.zst}" ;;
            *)        cp "$src" "$MODROOT/$rel" ;;
        esac
        awk -v m="$rel" 'index($0, m ":")==1 { sub(/^[^:]*: */,""); print }' \
            "/usr/lib/modules/$KVER/modules.dep" 2>/dev/null | tr ' ' '\n' | \
        while read -r dep; do
            [ -n "$dep" ] || continue
            [ -f "/usr/lib/modules/$KVER/$dep" ] || continue
            local dst="$MODROOT/${dep%.zst}"
            [ -f "$dst" ] && continue
            mkdir -p "$MODROOT/$(dirname "$dep")"
            case "$dep" in
                *.ko.zst) unzstd -q -c "/usr/lib/modules/$KVER/$dep" > "$dst" ;;
                *)        cp -n "/usr/lib/modules/$KVER/$dep" "$MODROOT/$dep" 2>/dev/null || true ;;
            esac
        done
    }

    # 802.11 core. cfg80211 and mac80211 are pulled in as deps, listed for clarity.
    copy_mod kernel/net/wireless/cfg80211.ko.zst
    copy_mod kernel/net/mac80211/mac80211.ko.zst
    copy_mod kernel/net/rfkill/rfkill.ko.zst

    # Intel: iwlwifi + the two opmodes (mvm for modern, dvm for old).
    copy_mod kernel/drivers/net/wireless/intel/iwlwifi/iwlwifi.ko.zst
    copy_mod kernel/drivers/net/wireless/intel/iwlwifi/mvm/iwlmvm.ko.zst
    copy_mod kernel/drivers/net/wireless/intel/iwlwifi/dvm/iwldvm.ko.zst

    # Realtek: rtw88 (802.11ac) and rtw89 (802.11ax).
    # MediaTek: mt76 family, which covers MT7921/MT7922/MT7925 - the cards in
    # most AMD laptops (HP Envy, Lenovo IdeaPad, ASUS Vivobook). An AMD laptop
    # essentially never has Intel wifi, so shipping only iwlwifi was wrong for
    # that whole class of machine.
    #
    # Default is now "all": the combined cost is ~6 MB of modules and ~17 MB of
    # firmware, which is cheap next to another reboot cycle spent guessing. A
    # narrower build is still available: WIFI_VENDORS=intel|realtek|mediatek.
    for vendor in $(echo "${WIFI_VENDORS:-all}" | tr ',' ' '); do
        case "$vendor" in
            all|realtek)
                for m in $(find "/usr/lib/modules/$KVER/kernel/drivers/net/wireless/realtek/rtw88" \
                                "/usr/lib/modules/$KVER/kernel/drivers/net/wireless/realtek/rtw89" \
                                -name '*.ko.zst' 2>/dev/null); do
                    copy_mod "${m#/usr/lib/modules/$KVER/}"
                done
                ;;
        esac
        case "$vendor" in
            all|mediatek)
                for m in $(find "/usr/lib/modules/$KVER/kernel/drivers/net/wireless/mediatek" \
                                -name '*.ko.zst' 2>/dev/null); do
                    copy_mod "${m#/usr/lib/modules/$KVER/}"
                done
                ;;
        esac
    done
    echo "    wifi vendors: ${WIFI_VENDORS:-all}"

    if [ "$MOD_FAIL" -ne 0 ]; then
        echo "WIFI BUILD FAILED: required module(s) missing" >&2
        exit 1
    fi

    # --- firmware -------------------------------------------------------------
    mkdir -p "$ROOT/usr/lib/firmware"
    # iwlwifi firmware: 203 flat files, 97.6 MB total. Two ways to select:
    #
    # DEFAULT (WIFI_FW=modinfo): copy exactly what this kernel's iwlwifi module
    # declares as its firmware. `modinfo -F firmware iwlwifi` lists every file
    # the driver can ask for, so this covers EVERY card the driver supports -
    # including the AX211 (family "so") that the old hand-written list missed -
    # at 14.5 MB instead of 97.6 MB. This is the correct default: it is derived
    # from the driver rather than guessed from marketing names.
    #
    # WIFI_FW=all: copy every blob (97.6 MB, 116 MB initramfs). Last resort.
    # WIFI_FW=families: use the FW_KEEP family list below.
    #
    # History: the earlier list used model names ("ax211", "ax200") which match
    # no iwlwifi firmware file at all - files are named by silicon family. An
    # AX211 therefore got no firmware, iwlwifi loaded, found no card to bind, and
    # the boot reported "wifi: no wireless interface". Deriving the list from
    # modinfo makes that class of mistake impossible.
    FW_MODE="${WIFI_FW:-modinfo}"
    if [ "$FW_MODE" = "all" ]; then
        cp /usr/lib/firmware/iwlwifi-*.zst "$ROOT/usr/lib/firmware/" 2>/dev/null || true
        echo "    iwlwifi firmware: all blobs"
    elif [ "$FW_MODE" = "modinfo" ] && command -v modinfo >/dev/null 2>&1; then
        FW_N=0
        FW_MISS=0
        for name in $(modinfo -F firmware iwlwifi 2>/dev/null | sort -u); do
            [ -n "$name" ] || continue
            # Host firmware is .zst-compressed and often a symlink into
            # intel/iwlwifi/. Copy the target, keeping the flat name the driver
            # asks for (firmware loader searches /usr/lib/firmware directly).
            for cand in "/usr/lib/firmware/$name.zst" "/usr/lib/firmware/$name"; do
                if [ -e "$cand" ]; then
                    cp -L "$cand" "$ROOT/usr/lib/firmware/$(basename "$cand")" 2>/dev/null \
                        && FW_N=$((FW_N + 1))
                    break
                fi
            done
        done
        echo "    iwlwifi firmware: $FW_N blobs from modinfo (WIFI_FW=all for everything)"
    else
        # Families, not model names. iwlwifi firmware is named by silicon family
        # (iwlwifi-<family>-<stepping>-<variant>-<ver>.ucode), NOT by marketing
        # name: an AX211 is family "so", an AX201 is "Qu", an AX210 is "ty".
        # The previous list used model names like "ax211" and "ax200", which
        # match no file at all - so those cards got no firmware and iwlwifi
        # loaded, found no card to bind, and reported "no wireless interface".
        #
        # so/gl = AX211/AX411/AX1675 class (CNVi, most 2021+ Intel laptops)
        # ty    = AX210/AX211 desktop parts
        # cc    = AX200/AX201
        # Qu    = AX201/AX211 CNVi variants
        # ma    = AX211/AX411 newer CNVi
        # 9000/9260/9560/8265/8260/7265/3168/... = older laptop parts
        FW_KEEP="1000 2000 3160 3168 5000 6000 6050 7260 7265 8000 8260 8265 9000 9260 9560 so gl ty cc ma"
        FW_N=0
        for g in $FW_KEEP; do
            for f in /usr/lib/firmware/iwlwifi-$g*.zst; do
                [ -f "$f" ] || continue
                cp "$f" "$ROOT/usr/lib/firmware/" 2>/dev/null && FW_N=$((FW_N + 1))
            done
        done
        echo "    iwlwifi firmware: $FW_N blobs from family list"
    fi

    # Regulatory database. cfg80211 loads it at init to apply per-country
    # channel/power limits. Without it you get
    #   cfg80211: failed to load regulatory.db
    # and the world-domain (00) rules apply, which disables channels the AP may
    # be using - a real cause of "iwd sees the network but cannot join it".
    for rf in regulatory.db regulatory.db.p7s; do
        [ -f "/usr/lib/firmware/$rf" ] && \
            cp "/usr/lib/firmware/$rf" "$ROOT/usr/lib/firmware/" 2>/dev/null || true
    done
    for fw in rtw88 rtw89; do
        case ",${WIFI_VENDORS:-all}," in
            *,all,*|*,realtek,*)
                [ -d "/usr/lib/firmware/$fw" ] && \
                    cp -r "/usr/lib/firmware/$fw" "$ROOT/usr/lib/firmware/" 2>/dev/null || true ;;
        esac
    done

    # MediaTek mt76 firmware. The whole /usr/lib/firmware/mediatek tree is 38 MB
    # and most of it is Bluetooth and SoC blobs. Only the WIFI_* and mt79*/mt76*
    # files are needed for the wifi driver.
    case ",${WIFI_VENDORS:-all}," in
        *,all,*|*,mediatek,*)
            if [ -d /usr/lib/firmware/mediatek ]; then
                mkdir -p "$ROOT/usr/lib/firmware/mediatek"
                for f in /usr/lib/firmware/mediatek/*.zst; do
                    [ -f "$f" ] || continue
                    case "$(basename "$f")" in
                        WIFI_*|mt76*|mt79*) cp "$f" "$ROOT/usr/lib/firmware/mediatek/" 2>/dev/null || true ;;
                    esac
                done
                echo "    mediatek firmware: $(ls "$ROOT/usr/lib/firmware/mediatek" | wc -l) files"
            fi ;;
    esac

    # --- iwd ------------------------------------------------------------------
    if [ -x /usr/lib/iwd/iwd ]; then
        install -m 755 /usr/lib/iwd/iwd "$ROOT/usr/sbin/iwd"
        cp -L /usr/lib/libell.so.0 "$ROOT/usr/lib/libell.so.0"
        # libgcc_s: iwd links against it and the base rootfs does not carry it.
        # Without it iwd dies at startup with
        #   error while loading shared libraries: libgcc_s.so.1
        # which is invisible unless you actually run the binary - the file being
        # present in the image proves nothing.
        for lib in libgcc_s.so.1 libm.so.6; do
            [ -f "/usr/lib/$lib" ] && cp -L "/usr/lib/$lib" "$ROOT/usr/lib/$lib" 2>/dev/null || true
        done
        # iwd reads its config from /etc/iwd; a minimal one is enough.
        mkdir -p "$ROOT/etc/iwd"
        printf '[General]\nEnableNetworkConfiguration=true\n' > "$ROOT/etc/iwd/main.conf"
    else
        echo "    WARNING: /usr/lib/iwd/iwd missing, no wifi supplicant" >&2
    fi

    # iwctl is the CLI front end; useful for debugging from the console.
    # It needs libreadline + libncursesw on top of iwd's own deps.
    if [ -x /usr/bin/iwctl ]; then
        install -m 755 /usr/bin/iwctl "$ROOT/usr/bin/iwctl"
        for lib in libreadline.so.8 libncursesw.so.6 libtinfo.so.6; do
            [ -f "/usr/lib/$lib" ] && cp -L "/usr/lib/$lib" "$ROOT/usr/lib/$lib" 2>/dev/null || true
        done
    fi

    # Verify every shared library iwd needs is actually in the rootfs.
    # A missing .so does not fail the build, it fails the boot - and only
    # when the binary is executed, which may be long after the image looked
    # fine. Check every binary we ship, not just iwd.
    IWD_MISSING=""
    for bin in /usr/lib/iwd/iwd /usr/bin/iwctl; do
        [ -x "$bin" ] || continue
        for so in $(ldd "$bin" 2>/dev/null | awk '/=> \//{print $3}'); do
            base="$(basename "$so")"
            case "$base" in
                libc.so.6|ld-linux-x86-64.so.2) continue ;;  # in base rootfs
            esac
            [ -f "$ROOT/usr/lib/$base" ] || [ -f "$ROOT/usr/lib64/$base" ] || \
                IWD_MISSING="$IWD_MISSING $base"
        done
    done
    if [ -n "$IWD_MISSING" ]; then
        echo "    WIFI BUILD FAILED: missing shared libs:$IWD_MISSING" >&2
        exit 1
    fi
    echo "    iwd installed (all shared libs present)"

    echo "    wifi modules: $(find "$MODROOT" -name '*.ko' | wc -l) total"
    echo "    firmware: $(du -sh "$ROOT/usr/lib/firmware" 2>/dev/null | cut -f1)"
fi

# --- 1d. graphical (optional, GUI=1) -----------------------------------------
# Makes the boot land in the NativeRa front-end instead of a busybox prompt.
#
# NO DESKTOP ENVIRONMENT. No X11, no Wayland, no compositor, no display manager.
# SDL2 has a KMSDRM backend that renders straight to the GPU framebuffer, which
# is what Batocera does and why it reaches its menu in seconds. A console distro
# with a desktop environment is a contradiction: it costs 300+ MB and buys
# nothing a fullscreen game launcher needs.
#
# What this adds:
#   - amdgpu (+ deps) and its firmware, filtered to Ryzen iGPU families
#   - i915 firmware, so Intel laptops work too
#   - SDL2 + SDL2_ttf + freetype/harfbuzz and their deps
#   - the nativera-fe binary, a font, and a /init that execs it
if [ "${GUI:-0}" = "1" ]; then
    echo "==> gui: enabled"
    KVER="$KERNEL_VERSION"
    MODROOT="$ROOT/lib/modules/$KVER"
    mkdir -p "$MODROOT"

    MOD_FAIL=0
    copy_mod() {
        local rel="$1"
        local src="/usr/lib/modules/$KVER/$rel"
        if [ ! -f "$src" ]; then
            echo "    MISSING MODULE: $rel" >&2
            MOD_FAIL=1
            return 0
        fi
        mkdir -p "$MODROOT/$(dirname "$rel")"
        case "$rel" in
            *.ko.zst) unzstd -q -c "$src" > "$MODROOT/${rel%.zst}" ;;
            *)        cp "$src" "$MODROOT/$rel" ;;
        esac
        awk -v m="$rel" 'index($0, m ":")==1 { sub(/^[^:]*: */,""); print }' \
            "/usr/lib/modules/$KVER/modules.dep" 2>/dev/null | tr ' ' '\n' | \
        while read -r dep; do
            [ -n "$dep" ] || continue
            [ -f "/usr/lib/modules/$KVER/$dep" ] || continue
            local dst="$MODROOT/${dep%.zst}"
            [ -f "$dst" ] && continue
            mkdir -p "$MODROOT/$(dirname "$dep")"
            case "$dep" in
                *.ko.zst) unzstd -q -c "/usr/lib/modules/$KVER/$dep" > "$dst" ;;
                *)        cp -n "/usr/lib/modules/$KVER/$dep" "$MODROOT/$dep" 2>/dev/null || true ;;
            esac
        done
    }

    # amdgpu pulls ~14 deps (ttm, gpu-sched, drm_display_helper, cec, wmi...).
    # copy_mod resolves them from modules.dep.
    copy_mod kernel/drivers/gpu/drm/amd/amdgpu/amdgpu.ko.zst
    # i915 for Intel laptops. Its deps are resolved the same way.
    copy_mod kernel/drivers/gpu/drm/i915/i915.ko.zst
    # vmwgfx drives VirtualBox's VMSVGA adapter. Without it a VM falls back to
    # simpledrm, which has no KMS driver behind it, and SDL's KMSDRM backend
    # reports "No available video device" even though /dev/dri/card0 exists.
    # This is what makes the graphical build testable in a VM at all.
    copy_mod kernel/drivers/gpu/drm/vmwgfx/vmwgfx.ko.zst

    if [ "$MOD_FAIL" -ne 0 ]; then
        echo "GUI BUILD FAILED: required GPU module(s) missing" >&2
        exit 1
    fi

    # --- GPU firmware ---------------------------------------------------------
    # amdgpu ships 699 files / 30 MB covering every AMD part since 2009. A Ryzen
    # 7 Envy has an integrated GPU from one of a handful of families; those are
    # 63 files / 0.9 MB. Ship the iGPU families plus a few common dGPUs.
    mkdir -p "$ROOT/usr/lib/firmware/amdgpu"
    FW_N=0
    if [ "${GPU_FW_ALL:-0}" = "1" ]; then
        cp /usr/lib/firmware/amdgpu/* "$ROOT/usr/lib/firmware/amdgpu/" 2>/dev/null || true
        FW_N=$(ls "$ROOT/usr/lib/firmware/amdgpu" | wc -l)
        echo "    amdgpu firmware: all ($FW_N files)"
    else
        # Ryzen iGPU families: raven/picasso (Zen+), renoir (Zen2), cezanne
        # (Zen3), rembrandt/phoenix (Zen4), yellow_carp (Zen5).
        for fam in raven picasso renoir cezanne rembrandt phoenix yellow_carp \
                   navi10 navi14 navi22 navi23 navi24 navi31 navi32 navi33 \
                   polaris10 polaris11 vega10 vega20; do
            for f in /usr/lib/firmware/amdgpu/${fam}_*; do
                [ -f "$f" ] || continue
                cp "$f" "$ROOT/usr/lib/firmware/amdgpu/" 2>/dev/null && FW_N=$((FW_N + 1))
            done
        done
        echo "    amdgpu firmware: $FW_N files (GPU_FW_ALL=1 for all)"
    fi
    # i915 firmware for Intel graphics.
    if [ -d /usr/lib/firmware/i915 ]; then
        mkdir -p "$ROOT/usr/lib/firmware/i915"
        cp /usr/lib/firmware/i915/*.zst "$ROOT/usr/lib/firmware/i915/" 2>/dev/null || true
    fi

    # --- SDL2 + font stack ----------------------------------------------------
    # SDL2 links only libc; SDL2_ttf pulls freetype, which pulls harfbuzz, png,
    # bz2, brotli, zlib. Copy the whole closure rather than guessing.
    #
    # libSDL3.so.0 is REQUIRED and ldd cannot see it: Arch's sdl2-compat is a
    # translation shim that dlopen()s libSDL3 at runtime, so the binary links
    # against libSDL2-2.0.so.0 but dies with "Failed loading SDL3 library" if
    # SDL3 is absent. A dependency check built on ldd alone will always miss it.
    GUI_LIBS=""
    for lib in libSDL2-2.0.so.0 libSDL3.so.0 libSDL2_ttf-2.0.so.0 libfreetype.so.6 \
               libharfbuzz.so.0 libpng16.so.16 libbz2.so.1.0 libbrotlidec.so.1 \
               libbrotlicommon.so.1 libz.so.1 libgcc_s.so.1 libm.so.6 \
               libglib-2.0.so.0 libgraphite2.so.3 libpcre2-8.so.0 \
               libgbm.so.1 libEGL.so.1 libGLESv2.so.2 libGLdispatch.so.0 \
               libdrm.so.2 libexpat.so.1; do
        src="/usr/lib/$lib"
        [ -e "$src" ] || continue
        cp -L "$src" "$ROOT/usr/lib/$lib" 2>/dev/null && GUI_LIBS="$GUI_LIBS $lib"
    done
    echo "    gui libs:$GUI_LIBS"

    # --- Mesa DRI + gallium ---------------------------------------------------
    # SDL3's KMSDRM backend renders through GBM + EGL, which loads a Mesa DRI
    # driver at runtime. Without these, video init fails with "No available video
    # device" even though /dev/dri/card0 exists and vmwgfx/amdgpu bound fine.
    #
    # All the *_dri.so names are symlinks to libdril_dri.so, which in turn pulls
    # libgallium (65 MB) - that is the real cost of a GPU path, and it is why a
    # distro ships Mesa as a package rather than inside an initramfs.
    mkdir -p "$ROOT/usr/lib/dri"
    cp -L /usr/lib/dri/libdril_dri.so "$ROOT/usr/lib/dri/" 2>/dev/null || true
    for d in radeonsi_dri.so iris_dri.so svga_dri.so kms_swrast_dri.so swrast_dri.so; do
        [ -e "/usr/lib/dri/$d" ] && ln -sf libdril_dri.so "$ROOT/usr/lib/dri/$d"
    done
    for g in /usr/lib/libgallium-*.so; do
        [ -f "$g" ] && cp -L "$g" "$ROOT/usr/lib/" 2>/dev/null || true
    done
    echo "    mesa: $(du -sh "$ROOT/usr/lib/dri" 2>/dev/null | cut -f1) dri + $(ls /usr/lib/libgallium-*.so 2>/dev/null | wc -l) gallium"

    # --- the front-end binary -------------------------------------------------
    FE="$REPO/engine/target/release/nativera-fe"
    if [ ! -x "$FE" ]; then
        echo "    building nativera-fe..."
        ( cd "$REPO/engine" && cargo build --release -p nativera_frontend >/dev/null 2>&1 ) || {
            echo "GUI BUILD FAILED: cargo build of nativera-fe failed" >&2
            exit 1
        }
    fi
    install -m 755 "$FE" "$ROOT/usr/bin/nativera-fe"

    # Font. The front-end looks in two fixed paths; ship one of them.
    mkdir -p "$ROOT/usr/share/fonts/TTF"
    for f in /usr/share/fonts/TTF/DejaVuSans-Bold.ttf \
             /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf; do
        [ -f "$f" ] && cp "$f" "$ROOT/usr/share/fonts/TTF/DejaVuSans-Bold.ttf" && break
    done

    # --- verify the front-end's own library closure ---------------------------
    # Same rule as iwd: a missing .so fails at run time, not build time, and only
    # when the binary is executed.
    FE_MISSING=""
    for so in $(ldd "$FE" 2>/dev/null | awk '/=> \//{print $3}'); do
        base="$(basename "$so")"
        case "$base" in
            libc.so.6|ld-linux-x86-64.so.2) continue ;;
        esac
        [ -f "$ROOT/usr/lib/$base" ] || [ -f "$ROOT/usr/lib64/$base" ] || \
            FE_MISSING="$FE_MISSING $base"
    done
    if [ -n "$FE_MISSING" ]; then
        echo "    GUI BUILD FAILED: nativera-fe missing shared libs:$FE_MISSING" >&2
        exit 1
    fi

    # dlopen()'d libraries are invisible to ldd. sdl2-compat loads libSDL3 at
    # runtime, so check it explicitly - this exact omission produced
    # "Failed loading SDL3 library" on a build that otherwise passed every check.
    DLOPEN_MISSING=""
    for so in libSDL3.so.0; do
        [ -f "$ROOT/usr/lib/$so" ] || DLOPEN_MISSING="$DLOPEN_MISSING $so"
    done
    if [ -n "$DLOPEN_MISSING" ]; then
        echo "    GUI BUILD FAILED: dlopen'd libs missing:$DLOPEN_MISSING" >&2
        exit 1
    fi
    echo "    nativera-fe installed (all shared libs present, incl. dlopen'd)"

    # Empty roms dir so the front-end shows its placeholder rather than failing.
    mkdir -p "$ROOT/roms"
fi

# --- 2. package: newc cpio + gzip, root-owned, symlinks preserved -----------
( cd "$ROOT" && \
  find . -print0 | cpio -o -0 -R 0:0 --format=newc 2>/dev/null | \
      gzip -9 > "$OUT/initramfs.cpio.gz" )
echo "==> initramfs: $(du -h "$OUT/initramfs.cpio.gz" | cut -f1)"

# --- 3. deploy to drive --------------------------------------------------------
mkdir -p "$DRIVE/EFI/BOOT"
cp "$LIMINE_EFI" "$DRIVE/EFI/BOOT/BOOTX64.EFI"
cp "$KMOD/vmlinuz" "$DRIVE/vmlinuz"
cp "$OUT/initramfs.cpio.gz" "$DRIVE/initramfs.cpio.gz"
cp "$SRC/limine.conf" "$DRIVE/limine.conf"
cp "$SRC/limine.conf" "$DRIVE/EFI/BOOT/limine.conf"

# --- 4. verify the deploy actually landed -------------------------------------
# A silent partial deploy is the worst failure mode: the stick looks fine and
# just does not boot. Fail loudly here instead.
echo "==> verifying deploy"
for f in EFI/BOOT/BOOTX64.EFI EFI/BOOT/limine.conf limine.conf vmlinuz initramfs.cpio.gz; do
    if [ ! -s "$DRIVE/$f" ]; then
        echo "DEPLOY FAILED: $DRIVE/$f missing or empty" >&2
        exit 1
    fi
    printf '    %-26s %10s bytes\n' "$f" "$(stat -c %s "$DRIVE/$f")"
done

sync
echo "==> deployed to $DRIVE"
echo "==> Boot: enter UEFI setup, select the USB stick first (Secure Boot OFF)."
echo "==> Expect: Limine menu -> 'NativeRa Prototype' -> kernel -> text shell banner."
