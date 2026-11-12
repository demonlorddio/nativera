# The Recomp Seam — how a native title plugs into Batocera's ES

Batocera's EmulationStation is **system-driven**, not emulator-driven. Every system (gb, gba, ports, windows, naomi, model3, ps3) uses the same launch command:

```
emulatorlauncher %CONTROLLERSCONFIG% -system %SYSTEM% -rom %ROM% -gameinfoxml %GAMEINFOXML% -systemname %SYSTEMNAME%
```

The **system** decides what the ROM is and what the core is. That's the seam.

## Proven: the `sh` generator is `/bin/bash run.sh`

Extracted from Batocera's rootfs (`configgen/generators/sh/shGenerator.py`):

- A **directory ROM** is first-class — the generator runs the folder's
  **`run.sh`** if it exists, else the bare `.sh` file.
- The command is literally **`["/bin/bash", shrom]`** — *no* `-system`/`-rom`
  args, so the script **self-locates** via `$(dirname "$0")`.
- Mouse mode = on, exit hotkey = `Alt+F4`, and it writes an SDL
  game-controller DB (`SDL_GAMECONTROLLERCONFIG`).

So **a native title = a directory containing `run.sh`**, and that `run.sh` *is*
the manifest consumer: `ES → configgen → /bin/bash run.sh`. Zero EmulationStation
patching. Full contract detail lives in `LAUNCHER.md`.

## The ports pattern (already-native x86)

- **System:** `ports`
- **ROMs:** `.sh` scripts in `/userdata/roms/ports/`
- **Core:** `sh`
- **Wrapper:** a bash script that sets up env, logs, and execs the native binary.

Example (`/userdata/roms/ports/Celeste64.sh`):
```bash
#!/bin/bash
export $(cat /proc/1/environ | tr '\0' '\n')
export DISPLAY=:0.0
app_dir="/userdata/system/add-ons/celeste64"
app_binary="${app_dir}/Celeste64"
log_dir="/userdata/system/logs"
log_file="${log_dir}/celeste64.log"
mkdir -p "${log_dir}"
exec &> >(tee -a "$log_file")
cd "${app_dir}"
./Celeste64 "$@"
```

The `.sh` is the ROM; the binary lives in `/userdata/system/add-ons/<title>/`.

## The recomp seam (zero ES patching)

We can reuse the **ports** system for Phase 1A, or add a new **recomp** system. Either way, the contract is identical:

1. **System entry** (in `es_systems.cfg`):
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

2. **ROM = the folder's `run.sh`** (`/userdata/roms/recomp/<title>/run.sh`).
   The `sh` generator runs it as `/bin/bash run.sh`, so it self-locates and
   drives the manifest consumer (the consumer ships in-tree as a native
   add-on; it's the same `engine/recomp-launcher.py`):
   ```bash
   #!/bin/bash
   # nativera folder-ROM manifest consumer — configgen's `sh` generator runs
   # this as `/bin/bash run.sh` (no -system/-rom), so it self-locates.
   set -u
   HERE="$(cd "$(dirname "$0")" && pwd)"                 # the title dir
   CONSUMER="/userdata/system/add-ons/recomp-launcher/recomp-launcher.py"
   PY=/usr/bin/python3
   export $(cat /proc/1/environ | tr '\0' '\n')          # inherit the boot env
   export DISPLAY="${DISPLAY:-:0.0}"
   GAME="${NATIVERA_GAME:-$HERE/game}"                    # D6: the user's BYO copy
   log_dir="/userdata/system/logs"; log_file="${log_dir}/$(basename "$HERE").log"
   mkdir -p "${log_dir}"
   NATIVERA_TITLE="$HERE" "$PY" -I "$CONSUMER" "$HERE" run --game "$GAME" \
     2>&1 | tee -a "$log_file"
   exit ${PIPESTATUS[0]:-0}
   ```
   The consumer then runs `verify → prepare → config → start → saves` against
   the `recomplauncher.json` in `$HERE`.

3. **Package layout** (`/userdata/roms/recomp/<title>/`):
   ```
   /userdata/roms/recomp/<title>/
     run.sh                  # folder-ROM consumer (what /bin/bash runs)
     recomplauncher.json     # the manifest (the consumer reads it)
     <binary>                # native executable
     rom_data/               # warm cache / assets
     game/                   # the user's BYO copy (D6) or a pointer to one
     saves/                  # user data
     config/                 # settings
   ```

## Phase 1A — quick win (no manifest, ports-style)

Pick an existing x86 title from `ports/`, `windows/`, `model3/`, or `naomi/` and drop it into `recomp/` unchanged. The wrapper is the `.sh`; the binary is already native. This proves the menu → launch → save path with **zero new code**.

## Phase 1B — manifest-driven (recomp system)

Add the manifest consumer to the wrapper:
- Read `recomplauncher.json` from the title dir.
- Run `prepare` (headless, progress lines).
- Write `config` (`ensure`/`always`/`new`).
- Exec `start.program` with `start.args` + injected env.
- Copy `saves[]` globs after the session.

The wrapper becomes a **manifest consumer** that speaks the RecompLauncher contract. Once it works, **every upstream manifest drops in and just works** — no per-game consumer code.

**Content caveat:** the consumer is manifest-agnostic, but the *titles we ship
are our own native ports* (no emulator underneath). Asura's Wrath (RPCS3-based)
is only a consumer self-test fixture, not shipped nativera content — the first
shipped native title is our flagship **Game Boy / GBA** port.

## Phase 2 — dedicated recomp system

Fork/patch EmulationStation to add a **native provider** that reads `recomplauncher.json` directly (no `.sh` wrapper). This is cleaner, but Phase 1A/1B already ship without it.
