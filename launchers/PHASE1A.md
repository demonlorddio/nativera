# Phase 1A — Quick Win: ports → recomp (zero code, zero patching)

**Goal:** prove that a native title can appear in the Batocera menu and launch, using the existing `ports` pattern, with **zero EmulationStation or configgen patching**.

## Candidate: BatoDesktop

- **Wrapper:** `/run/media/dio/SHARE/roms/ports/BatoDesktop.sh` (4 lines)
- **Binary:** `/userdata/system/add-ons/desktop/Desktop.AppImage` (x86_64 AppImage)
- **Complexity:** trivial — just execs an AppImage with mouse+scale env.

## Path model (host ↔ in-boot)

The ext4 `SHARE` partition is `/run/media/dio/SHARE` on the host, mounted at
`/userdata` **in-boot**. Translate as: host path = `/run/media/dio/SHARE/` +
(in-boot path minus its `/userdata/` prefix).

| in-boot (the system's view) | host (your machine, rw) |
|---|---|
| `/userdata/system/add-ons/desktop/Desktop.AppImage` | `/run/media/dio/SHARE/system/add-ons/desktop/Desktop.AppImage` |
| `/userdata/system/configs/emulationstation/es_systems.cfg` | `/run/media/dio/SHARE/system/configs/emulationstation/es_systems.cfg` |
| `/userdata/roms/recomp/<title>/…` | `/run/media/dio/SHARE/roms/recomp/<title>/…` |

## Steps (read-only dry-run first, then apply)

### 1. Dry-run: confirm the file tree
```bash
# on the host (your machine)
ls -la /run/media/dio/SHARE/roms/ports/BatoDesktop.sh
ls -la /run/media/dio/SHARE/system/add-ons/desktop/Desktop.AppImage
```

### 2. Create the recomp system entry

We have two options:
- **Option A (fastest):** reuse the existing `ports` system, drop a new `.sh` into `/userdata/roms/ports/` (already works).
- **Option B (cleaner):** add a new `recomp` system entry to `es_systems.cfg` + drop the `.sh` into `/userdata/roms/recomp/`.

**Pick Option B** — it keeps ports untouched and proves the new system.

#### 2.1 Add the recomp system entry

The `es_systems.cfg` lives inside the rootfs archive (`boot/batocera`). To add a system without rebuilding the archive, we can:
- **Option B1 (non-persistent):** add the entry to `/userdata/system/configs/emulationstation/es_systems.cfg` (Batocera merges user configs over the rootfs).
- **Option B2 (persistent):** unpack the archive, add the entry, repack. Heavier.

**Pick B1** — non-persistent is fine for the proof; we can persist it later.

Create `/userdata/system/configs/emulationstation/es_systems.cfg` (if it doesn't exist) and add:
```xml
<system>
  <fullname>Recompiled Games</fullname>
  <name>recomp</name>
  <manufacturer>nativera</manufacturer>
  <release>2026</release>
  <hardware>native</hardware>
  <path>/userdata/roms/recomp</path>
  <extension>.sh</extension>
  <command>emulatorlauncher %CONTROLLERSCONFIG% -system recomp -rom %ROM% -gameinfoxml %GAMEINFOXML% -systemname %SYSTEMNAME%</command>
  <platform>pc</platform>
  <theme>recomp</theme>
  <group>ports</group>
  <emulators>
    <emulator name="sh">
      <cores>
        <core default="true">sh</core>
      </cores>
    </emulator>
  </emulators>
</system>
```

#### 2.2 Create the recomp ROM directory
```bash
mkdir -p /run/media/dio/SHARE/roms/recomp
```

#### 2.3 Copy the ports wrapper into recomp
```bash
cp /run/media/dio/SHARE/roms/ports/BatoDesktop.sh /run/media/dio/SHARE/roms/recomp/BatoDesktop.sh
chmod +x /run/media/dio/SHARE/roms/recomp/BatoDesktop.sh
```

#### 2.4 Confirm the binary exists
```bash
ls -la /run/media/dio/SHARE/system/add-ons/desktop/Desktop.AppImage
```

### 3. Boot and test

- Boot the Batocera HDD (physical or QEMU).
- Navigate to the **Recompiled Games** system.
- Select **BatoDesktop** → it should launch the Desktop AppImage.
- Confirm: appears in menu → launches natively → exits cleanly.

### 4. Deliverable

- `BOOT-PROOF.md` with:
  - the exact `es_systems.cfg` entry used
  - the wrapper script contents
  - the menu → launch trace (screenshot or log)
  - confirmation that it runs natively (no emulator in the path)

## Rollback

If anything breaks:
```bash
rm -rf /run/media/dio/SHARE/roms/recomp
rm -f /run/media/dio/SHARE/system/configs/emulationstation/es_systems.cfg
```

## Next: Phase 1B (manifest-driven)

Once Phase 1A works, replace the wrapper with a **manifest consumer** that reads `recomplauncher.json` and implements the full contract (prepare → config → start → saves). Then drop a **native port title** (our flagship Game Boy / GBA port) into `/userdata/roms/recomp/` and confirm it boots natively. Keep **Asura's Wrath** as a consumer self-test fixture only — it's RPCS3-based (an emulator underneath), which violates our no-emulator content rule, so it is **not** shipped as nativera content.
