# The RecompLauncher contract (the "launcher part")

Everything below is extracted from the authoritative source
`https://recomplauncher.com/sdk-files/guide.md` (the RecompLauncher SDK "guide"),
cross-checked against the real Asura's Wrath manifest
(`AsurasWrathRecomp-linux-x86_64/recomplauncher.json`, `format: 1`) and against
Batocera's own `configgen` `sh` generator (see `SEAM.md`).

RecompLauncher is a **distribution/manifest standard**, not a toolchain: a title
is *a native recompiled executable + the player's own assets + one
`recomplauncher.json`*. Any consumer that speaks the manifest makes every
upstream recomp drop-in. Our consumer is `engine/recomp-launcher.py`; this doc
is the spec it implements.

---

## 1. The manifest (`recomplauncher.json`, `format: 1`)

Top-level fields:

| field | meaning |
|---|---|
| `$schema` | editor autocompletion URL; ignored at runtime. |
| `format` | must be `1`. |
| `repo` | GitHub `owner/name` whose **Releases** supply the install/update. |
| `name` / `game` | display name; `game` = `{kind, label, check, dumps}`. |
| `start` | how to launch: `{program[], args[], env{}}`. |
| `config[]` | settings write plan: `{id, format, file, new/ensure/always}`. |
| `settings[]` | UI choices: `{id,label,help,choices[],defaults,when}`. |
| `saves[]` | user-data globs copied back after each session. |
| `games[]` | multi-title bundle: `{id,name,args,title_id,inputs}`. |
| `prepare` | one-time native build/cache: `{program,args,label,timeout}`. |
| `folders[]` / `actions[]` | openable/clearable dirs; actions `open`/`clear`. |
| `exit_codes{}` | map **exit-code → player-facing string**. |
| `looks[]` | ReShade look presets / forced graphics API. |
| `mods[]` | mod provisioning (`thunderstore`/`gamebanana`, `loader`). |
| `inputs[]` | runtime file provisioning (e.g. DLSS). |

### `game`
- `kind`: `"iso"` / `"folder"` / `"iso_or_folder"` — what the player drops in.
- `label`: the human prompt ("Your own copy of X…").
- `check`: **one of `file` / `xdvdfs` / `iso9660`** → the value is a path/bytes
  the launcher **matches inside the player's copy** to confirm it's the right
  game. (Our consumer's `verify()` loops these three keys.)
- `dumps[]`: `[{name,size,crc32}]` from Redump — present ⇒ "verified" badge.
  Purely informational; the consumer does not gate on it.

### `start`
- `program[]`: **ordered** candidate executable paths; the first that exists
  wins (absolute or `{install}`-relative or a bare name on `$PATH`).
- `args[]`: static args the player may append to.
- `env{}`: env for the launch. **System names are blocked** (e.g. `PATH`,
  `LD_PRELOAD`, `HOME`, `NODE_OPTIONS`, …) so a manifest can't redirect the
  dynamic loader or shell. The consumer drops any key in `_BLOCKED_ENV`.

### `config`
- `id` (default `"main"`), `format` (`toml`/`ini`/`json`), `file`, `game_path`.
- Three **write disciplines**, lowest precedence first:
  - `new` — fill only when the file is absent/empty (holds the shipped defaults).
  - `ensure` — fill missing/empty keys, keep existing.
  - `always` — overwrite unconditionally every launch.

### `settings` (UI-facing)
- `choices[]`: each has `label` + either `set` (keys written into config) or
  `args` (cmdline args when `"in":"args"`). `defaults{}`, `when` (gate).

### `saves`
- Folders / files / `*` globs. After each session the **launcher copies the
  ten newest** (and may sync to the account). Anything **> 64 MB** or binary
  game assets is excluded. Our consumer copies the globs into
  `<install>/.recomp/saves/` and skips files over 64 MB.

### `games` (multi-title bundle)
- `id` (immutable — saves/achievements key on it), `name`, `args`,
  `title_id` (Xbox 360 achievement-unlock file), `inputs`.

### `prepare`
- `{program, args, label, timeout}`. Runs on the **setup screen, after the copy
  check and before config writes**; re-runs whenever the disc/folder is swapped.
  Must **not open a window**. Emits **`progress <0–1> <text>`** lines.
  Exit `0` = success; any other code is shown as a failure with that code.

### `folders` / `actions`
- `folders`: openable/clearable dirs; the newest `.log` attaches to crash
  reports. `actions`: `"open"` / `"clear"`.

### `exit_codes`
- Numeric code → player string, e.g. `252968961` →
  "It couldn't find your copy of the game. Pick it again on the game's page."
  The consumer looks up `str(returncode)` and prints the string when present.

### `looks`
- `graphics_key`, `in`, `graphics` (force an API string), `lines`,
  `allowed`/`reason` (ReShade look presets). Ignored by the headless consumer.

### `mods`
- `thunderstore` + `data`, `gamebanana` (numeric ID), `loader`
  (`nrm`/`otr`/`hmm`/`user`). Ignored by the headless consumer.

### `inputs` (runtime file provisioning)
- `{id,label,role,shared,optional,destination}` (e.g. a DLSS DLL). The client
  downloads it from the vendor repo, verifies the signature, caches **one**
  copy; when absent, any arg referencing `{input:<id>}` is **omitted** (the
  consumer returns "" for that arg and drops the empty string).

### Placeholders
| token | meaning |
|---|---|
| `{install}` | the game's own install folder |
| `{game}` | the player's disc image / folder |
| `{documents}` | user documents dir |
| `{local}` | `%LOCALAPPDATA%` / `~/.local/share` |
| `{config}` | `%APPDATA%` / `~/.config` |
| `{home}` | home dir |
| `{pad}` | **`xbox` / `playstation`** (a controller *type*, not an index) |
| `{input:<id>}` | the provisioned `inputs` file path |

A file entry may be a **list with `if` guards**; first match wins, an unguarded
entry is the fallback.

### Environment injected on **every** launch
```
RECOMPLAUNCHER=1
RECOMPLAUNCHER_VERSION=<semver, e.g. 0.75.0-beta.1>
RECOMPLAUNCHER_MODE=<bigpicture|desktop>
RECOMPLAUNCHER_PAD=<xbox|playstation>
```
(Our consumer also injects `RECOMPLAUNCHER_INSTALL` / `RECOMPLAUNCHER_GAME`
for the native title's convenience.)

### End-to-end client flow
1. **install/update** — GitHub Releases + SHA-256 verify + unpack that
   preserves the player's writes.
2. **setup** — drop disc → read → match `game.check` → run `prepare` → write
   `config` per `new`/`ensure`/`always`.
3. **play** — resolve `start.program`, assemble `args`, set `env`, inject the
   four `RECOMPLAUNCHER_*` vars, `exec`.
4. **during play** — ReShade `looks`.
5. **on exit** — copy `saves`, print the friendly `exit_codes` message, attach
   the newest log.

---

## 2. The Batocera `sh`-system seam (how a title actually launches)

Batocera's `configgen` **`sh` generator** (`configgen/generators/sh/shGenerator.py`,
proven from the rootfs squashfs) is the zero-patch seam. Key facts:

- A **directory ROM** is first-class: if the ROM path is a folder, the launcher
  runs that folder's **`run.sh`** (`rom/"run.sh"`); if it's a bare file, it
  runs the `.sh` itself.
- The launch command is literally **`["/bin/bash", shrom]`** — *no*
  `-system`/`-rom` args. The script **self-locates** (`$(dirname "$0")`).
- Mouse mode = on; exit hotkey = `Alt+F4`; the generator writes an
  SDL game-controller DB (`SDL_GAMECONTROLLERCONFIG`).

So a **native title = a directory containing `run.sh`**, and that `run.sh`
*is* the manifest consumer: `ES → configgen → /bin/bash run.sh`. No EmulationStation
patching. The canonical real wrapper is `/userdata/roms/ports/Celeste64.sh`
(env export from `/proc/1/environ`, `DISPLAY=:0.0`, log to
`/userdata/system/logs/`, `exec` the native binary). See `SEAM.md`.

---

## 3. How our consumer reconciles

`engine/recomp-launcher.py` implements `verify → prepare → config → start →
saves` and was brought into line with the guide in this pass:

- ✅ `verify` checks exactly `file`/`iso9660`/`xdvdfs` (trimmed the
  non-contract `iso`/`dump` keys).
- ✅ `start.env` blocks the loader/shell names (`_BLOCKED_ENV`).
- ✅ injects `RECOMPLAUNCHER_VERSION` / `_MODE` / `_PAD` alongside `_INSTALL`/`_GAME`.
- ✅ `{pad}` defaults to `xbox` (a type, not an index).
- ✅ an absent `{input:<id>}` yields an empty arg that `start()` drops.
- ✅ `saves` skips files over 64 MB.
- ✅ `prepare` inherits the child's stdout so `progress <0–1> <text>` lines
  reach the console and the wrapper's `/userdata/system/logs` tee.
- ➖ `settings` / `looks` / `mods` / `folders` / `actions` are **UI/store**
  concerns — the headless consumer is aware of them but doesn't drive them.
  (Phase 2 ES provider may surface `settings`.)

Self-test: `python3 -I engine/test-recomp-launcher.py` — **10/10** checks pass
against the real Asura manifest (verify present/missing, `new` idempotence,
`saves` collection, `{config}` resolution, exit-code mapping).

### Known deltas vs. the desktop client (intentional)
- The desktop client does GitHub-release install/update + SHA-256; a bootable
  distro ships the binary in-tree, so `repo` is not yet driven.
- "Ten newest" `saves` is a desktop-account-sync convenience; the distro copies
  all matching globs (capped at 64 MB) into `<install>/.recomp/saves/`.

### Open (native-`recomp` model — Tier B GB/GBA)
The recomp model maps onto our own native ports: **`prepare`** = JIT/AOT-compile
the ROM's code regions into a native cache; **`start`** = run the cache;
**`saves`** = SRAM/flash. The GB/GBA ROM battery (1,958 `.gb` + 283 `.gba`
under `SHARE/roms/`) is the regression set for that flagship port. The
flagship scaffold `engine/gb/gbcore.c` is a **broken draft** (misremembered
opcode table + struct/IO-map/const bugs) and must be rebuilt from a verified
opcode table before use — do not trust it or the garbled web-sourced table.
