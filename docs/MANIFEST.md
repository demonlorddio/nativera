# The Manifest Layer

The spine of the catalogue: **`recomplauncher.json`** (schema fixed by recomplauncher.com, `format: 1`). A folder of files becomes a *launchable title* only when this file describes it.

## The key realization

RecompLauncher (upstream) is already a **consumer** of this manifest — it does the check → prepare → config → launch → save dance. If nativera's front-end/provider speaks the *same* manifest, then:

> **any recomp already submitted to RecompStore / GitHub Releases drops into our catalogue and works** — we build the manifest *consumer* once, not a wrapper per game.

That is what makes nativera a **distribution** and not a toolchain. Our per-console engines (Phase 3) are only for the families that have *no* upstream recomp yet.

## Manifest → nativera mapping (Batocera layout)

The manifest's path placeholders resolve onto our `/userdata` tree:

| Placeholder | Windows | Linux/desktop | **nativera (Batocera)** |
|---|---|---|---|
| `{install}` | per-install | per-install | `/userdata/recomp/<title>/` |
| `{game}` | user's disc/folder | user's | BYO disc/folder (see `game.kind`) |
| `{documents}`/`{local}` | `AppData\Local` | `~/.local/share` | `/userdata/recomp/<title>/data` |
| `{config}` | `AppData\Roaming` | `~/.config` | `/userdata/recomp/<title>/config` |
| `{home}` | user home | user home | `/home` or root home |
| `{input:<id>}` | via `inputs[]` | | resolved inputs |

**Native consumer contract** (what nativera's provider must implement, in order):
1. Read `recomplauncher.json` from the title dir.
2. `game`: check the BYO copy (`check.iso9660` / `check.xdvdfs` / `check.file`); compare `dumps[]` size/crc32 for a "verified" badge.
3. `prepare` (if present): run the pre-start step headless, show `progress <0..1> <text>` lines, gate on exit 0, re-run when the copy is swapped.
4. `config`: write `ensure`/`always`/`new` into the config file before each start; rewrite `game_path` to the user's copy.
5. `start`: exec `program` (first existing) with `args` + injected env (`RECOMPLAUNCHER=1`, `RECOMPLAUNCHER_MODE=bigpicture`, `RECOMPLAUNCHER_PAD`).
6. `saves`: after the session, copy the newest ≤10 matching `saves[]` globs.
7. `exit_codes`: translate the decimal exit code to a player-facing message.

## How it maps onto the proven Asura's Wrath work (PS3 reference)

| Manifest field | Asura value (from the working package) |
|---|---|
| `game.kind` | `iso_or_folder` (disc or `disc_extracted/`) |
| `game.check.iso9660` | BLUS30721 iso top-folder file |
| `game.dumps` | Redump name/size/crc32 |
| `prepare` | **LLVM precompile, 5–15 min, with progress lines** → now the PPU-cache *hydrate* fast path |
| `start.program` | `engine/rpcs3` (+ `asuraswrath` wrapper that sets `PPU Modules Mode: Prefer`) |
| `start.env` | `PPU Modules Dir` → `/userdata/recomp/asuraswrath/rom_data/` |
| `config` ensure/always/new | fixes the **P1 stale-in-memory-state** bug class; `always` = what the recomp needs to run |
| `saves` | `dev_hdd0/home/*/savedata/BLUS30721-*` + `dev_hdd0/game/BLUS30721/USRDIR` |
| `folders` | `rom_data`, `disc_extracted` |
| `exit_codes` | pin-mismatch → **"delete rom_data and rebuild"** |

## Full field reference (verbatim, format 1)

Top level: `$schema` (fixed URL), `format` (int `1`), `repo` (`owner/repo`), `name?`, `game`, `start`, `saves?`, `games?`, `config?`, `settings?`, `mods?`, `folders?`, `actions?`, `exit_codes?`, `prepare?`, `inputs?`, `looks?`.

- **`game`**: `kind` (`iso`|`folder`|`iso_or_folder`), `label`, `check` (one of `file`/`xdvdfs`/`iso9660`), `dumps[]`(`name`,`size`,`crc32`=8-hex).
- **`start`**: `program` (string[] tried in order, or file-list `[{path,if?}]`), `args` (string[], placeholders), `env` (string→string; system vars like `PATH` rejected).
- **`saves`**: string[] of folders / files / `*` globs; newest 10 kept; ≤64 MB; excludes game files/archives.
- **`games`** (multi-title in one release, needs launcher ≥0.90 Beta 36): `id` ([a-z0-9], ≤16, immutable), `name`, `args`, `title_id`, `inputs`.
- **`config`** (array): `id?` (default `main`), `format` (`toml`|`ini`|`json`), `file` (str or file-list), `game_path?`, `ensure`/`always`/`new` (key→value; verbatim).
- **`settings`** (array): `id`,`label`,`help?`,`in?` (config id or literal `args`), `choices[]`(`label`,`set?`,`args?`,`when?`), `defaults?`.
- **`mods`**: `thunderstore`,`data`,`gamebanana`,`loader`(`nrm`|`otr`|`hmm`|`user`).
- **`folders`**: named dirs the launcher can open/clear; `logs` key surfaces in crash reports.
- **`actions`**: settings buttons; value `open` (folder name) or `clear` (`saves/cache` subpath).
- **`exit_codes`**: object, keys = **decimal** strings, values = messages.
- **`prepare`**: `program`(string[]),`args?`,`label?`,`timeout?`(sec); runs after copy-check before config writes; re-runs on copy swap; `progress <0..1> <text>` lines; exit 0 = ok.
- **`inputs`**: `id`,`label`,`role?`,`shared?`,`optional?`,`destination?` (referenced via `{input:<id>}`).
- **`looks`**: `graphics_key?`,`in?`,`graphics?`,`lines?`,`allowed?`,`reason?`.

## Packaging / distribution conventions (RecompStore)

- Release naming: `Name-vMAJOR.MINOR.PATCH-OS-ARCH.ext` — e.g. `…-Linux-x64.tar.gz`/`.AppImage`, `…-macos-arm64.zip`, `…-Windows-x64.zip`.
- `recomplauncher.json` must sit at the **top of the zip, next to the executable**; a single top-level wrapper folder is auto-unwrapped.
- Excluded names: containing `debug`/`symbols`/`pdb`/`source`; ending `.sha256`/`.sig`/`.asc`.
- Integrity: GitHub SHA-256 verified before install. **Updates unpack over the existing folder; saves/settings/caches survive** (a settings file *inside* the release overwrites the player's — ship defaults via `config.new` instead).
- Pre-releases = opt-in "beta" channel.
- Consumer: RecompLauncher (desktop/BigPicture), cross Win/Linux/Mac, PS5 store.
