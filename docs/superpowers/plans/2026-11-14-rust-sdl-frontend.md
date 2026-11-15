# Rust SDL Front-End Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the C SDL front-end (`engine/launcher/launcher.c` + `launch.c`) to Rust as a new workspace member `engine/frontend`, byte-faithful in ROM scanning, UI constants, and shell-free launch semantics.

**Architecture:** A lib+bin crate `nativera_frontend` (bin `nativera-fe`) in the existing `engine/` cargo workspace. The lib holds the testable pieces — `roms` (directory scan), `launch` (shell-free child spawn + exit-status mapping), `ui` (pure layout constants/key handling/frame render); `main.rs` is a thin SDL event loop wiring them. System SDL2/SDL2_ttf link via pkg-config; the `sdl2` crate's typed `ttf` module (feature `ttf`) covers all font work — no raw FFI, no `unsafe` in our code.

**Tech Stack:** Rust (edition 2021, cargo 1.99), `sdl2 = { version = "0.38", features = ["use-pkgconfig", "ttf"] }` only. Host has SDL2 2.32.74 + SDL2_ttf 2.24.0 dev packages.

**Spec:** the C front-end itself is the spec: `engine/launcher/launcher.c` (UI/scan/loop), `engine/launcher/launch.c` + `launch_test.c` (launch contract), and the user-decided migration order consumer → front-end → core recorded in `docs/PLAN.md` / the nativera skill.

## Global Constraints

- UI constants are copied verbatim from `launcher.c`: bg `(24,26,33)`; header `(40,44,52)` rect `(0,0,1280,80)`; highlight `(65,105,225)` rect `(40, y-5, 1200, 40)`; rows at `y = 120 + i*50`, names at `(60, y)`; title "NativeRa - Game Library" at `(40, 25)`; selected text `(255,255,255)`, unselected `(180,180,180)`; font 24pt; frame pacing `sleep(16ms)` (C's `SDL_Delay(16)`).
- Launch contract (from `launch.c`): `./run.sh <rom>` as raw `argv[1]`, **never through a shell**; child exit → `0–255`; killed by signal → `128 + signum`; spawn/wait failure → `Err` (C's `-1`). Tests must pin a shell-injection payload ROM path arriving byte-for-byte.
- ROM scan semantics from `launcher.c`: files (and symlinks to files) whose name ends in `.gb` or `.sh`, case-sensitive; directories excluded; cap at 100 entries (`MAX_GAMES`); empty scan → single placeholder entry with name `"No ROMs found (Place .gb or .sh in roms/)"` and empty path; ROM dir = `argv[1]` else `./roms`.
- Font paths, in order: `/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf`, then `/usr/share/fonts/TTF/DejaVuSans-Bold.ttf`, at 24pt. If both fail to load, the UI still renders (background + header + highlight) but all text is skipped — the C behavior.
- No `unsafe` in `engine/frontend` source. No new dependencies beyond `sdl2` with the two features above.
- The C front-end (`engine/launcher/`) is **kept** in this plan; its removal happens after the GB core migration, not here.
- Every commit message ends with: `Co-Authored-By: Claude Code <noreply@anthropic.com>`

## Review Focus

Input classes the spec implies but no task's tests exercise, most likely to bite first:

1. **Font file absent on a minimal rootfs** — expected: textless UI (rects only), no crash. Owned by Task 4 (font-`None` frame test).
2. **ROM dir missing/unreadable** — expected: placeholder entry, same as C's failed `opendir`. Owned by Task 2.
3. **ROM filename with non-UTF-8 bytes** — expected: launch path stays byte-exact (OsString end-to-end); display name may be lossy. Owned by Tasks 2 + 3.
4. **> 100 ROM files** — expected: first 100 in readdir order, no panic, no silent growth past the cap. Owned by Task 2.
5. **`run.sh` absent in CWD** — expected: clean error line to stderr, loop continues (C printed `[launch] could not start ...` and kept running). Owned by Task 4.

---

### Task 1: Crate scaffold + headless SDL smoke

**Files:**
- Modify: `engine/Cargo.toml` (add `frontend` to `members`)
- Create: `engine/frontend/Cargo.toml`, `engine/frontend/src/lib.rs`, `engine/frontend/src/main.rs`, `engine/frontend/tests/sdl_smoke.rs`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: crate `nativera_frontend` (lib, empty modules `roms`, `launch`, `ui` declared in Task 1 as stub files so later tasks only edit them) and bin `nativera-fe`; the smoke test proves `sdl2` 0.38 + `use-pkgconfig` + `ttf` features compile, link against system SDL2/SDL2_ttf, and initialize headless.

- [ ] **Step 1: Write the failing smoke test**

`engine/frontend/tests/sdl_smoke.rs` — one test, `sdl_ttf_init_headless`. A `static ONCE: std::sync::Once` guards `std::env::set_var` so parallel tests don't race it (edition 2021: `set_var` is a safe fn; wrap it in `unsafe` only if the workspace ever moves to edition 2024):

```rust
static ONCE: std::sync::Once = std::sync::Once::new();

#[test]
fn sdl_ttf_init_headless() {
    ONCE.call_once(|| std::env::set_var("SDL_VIDEODRIVER", "dummy"));
    let sdl = sdl2::init().unwrap();
    let video = sdl.video().unwrap();
    let _ttf = sdl2::ttf::init().unwrap();
    let window = video
        .window("NativeRa Frontend", 1280, 720)
        .position_centered()
        .build()
        .unwrap();
    let _canvas = window.into_canvas().accelerated().present_vsync().build().unwrap();
}
```

Each `.unwrap()` is one required Ok-assertion: SDL init, video subsystem, ttf init, window, canvas.

- [ ] **Step 2: Run it and watch it fail**

Run: `cargo --manifest-path engine/Cargo.toml test -p nativera_frontend`
Expected: fails to resolve package `nativera_frontend` (crate doesn't exist yet).

- [ ] **Step 3: Scaffold the crate**

`engine/frontend/Cargo.toml`: `[package] name = "nativera_frontend"`, edition 2021, version 0.1.0; `[lib] name = "nativera_frontend"`; `[[bin]] name = "nativera-fe", path = "src/main.rs"`; `[dependencies] sdl2 = { version = "0.38", features = ["use-pkgconfig", "ttf"] }`. `src/lib.rs`: `pub mod roms; pub mod launch; pub mod ui;` plus empty `src/roms.rs`, `src/launch.rs`, `src/ui.rs` stubs. `src/main.rs`: `fn main() {}` with a comment that Task 4 fills it. Add `"frontend"` to `members` in `engine/Cargo.toml`.

- [ ] **Step 4: Run the test and watch it pass**

Run: `cargo --manifest-path engine/Cargo.toml test -p nativera_frontend`
Expected: `sdl_ttf_init_headless` PASS. (If link fails, `sdl2-config`/`sdl2_ttf` pkg-config files are missing — stop, report to the user; do not switch to the `bundled` feature without a ruling.)

- [ ] **Step 5: Commit**

```bash
git add engine/Cargo.toml engine/frontend
git commit -m "fe: scaffold nativera_frontend crate (sdl2 0.38 + use-pkgconfig + ttf, headless smoke test)

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: ROM scan module

**Files:**
- Create: `engine/frontend/src/roms.rs` (replaces Task 1's stub; unit tests inline in the same file)

**Interfaces:**
- Consumes: Task 1's crate.
- Produces (used verbatim by Tasks 3/4):
  - `pub struct GameEntry { pub name: String, pub path: std::ffi::OsString }` — `name` is the display string (lossy-converted, see Ruling R2), `path` is the full `dir/joined` byte-exact path.
  - `pub const PLACEHOLDER_NAME: &str = "No ROMs found (Place .gb or .sh in roms/)";`
  - `pub fn scan(dir: &std::path::Path) -> Vec<GameEntry>` — returns `vec![]` when `dir` is missing/unreadable.
  - `pub fn with_placeholder(games: Vec<GameEntry>) -> Vec<GameEntry>` — returns `games` if non-empty, else `vec![GameEntry { name: PLACEHOLDER_NAME.into(), path: OsString::new() }]`.

- [ ] **Step 1: Write the failing tests**

In `engine/frontend/src/roms.rs` (empty module first, tests fail to compile = red): using `std::env::temp_dir()` + a unique subdir per test (suffix `process::id()`), fixture files `a.gb`, `b.sh`, `c.txt`, `UPPER.GB`, and a **directory** named `tricky.gb`; plus a symlink `link.gb -> a.gb` (skip symlink assertions on non-Unix with `#[cfg(unix)]`). Tests:
  - `scans_gb_and_sh`: exactly the entries for `a.gb` and `b.sh` (and `link.gb` on Unix); `c.txt` and `UPPER.GB` absent; `tricky.gb` dir absent.
  - `path_is_dir_joined`: entry `path` for `a.gb` == `dir/a.gb` as OsString.
  - `empty_dir_gets_placeholder`: `scan` on empty dir → `vec![]`; `with_placeholder` of that → one entry, `name == PLACEHOLDER_NAME`, `path.is_empty()`.
  - `missing_dir_is_empty`: `scan(dir/does-not-exist)` → `vec![]`.
  - `cap_at_100`: write 105 files `g{i}.gb` → `scan` returns exactly 100.
  - `non_utf8_name_survives`: create file via `std::os::unix::ffi::OsStringExt` from `b"caf\7f.gb"`; `scan` finds it; `entry.path` equals the exact joined bytes; `entry.name` is a lossy String (assert `entry.name.ends_with(".gb")`).

- [ ] **Step 2: Run tests, watch them fail**

Run: `cargo --manifest-path engine/Cargo.toml test -p nativera_frontend roms`
Expected: FAIL — `scan`/`with_placeholder`/`GameEntry` don't exist.

- [ ] **Step 3: Implement `scan` and `with_placeholder`**

`scan`: `dir.read_dir()` → `Ok` entries else `return vec![]`; for each entry keep it iff `entry.file_type().map(|ft| ft.is_file()).unwrap_or(false)` (follows symlinks, matching C's DT_REG/DT_LNK; a *broken* symlink's `file_type()` errors and is dropped — a dangling ROM link is useless, and the test fixtures only use a live symlink) **and** its file name's extension (case-sensitive `OsStr`) is `"gb"` or `"sh"`; build `GameEntry { name: entry.file_name().to_string_lossy().into_owned(), path: entry.path().into_os_string() }`; stop adding at 100.

Rulings: **R1** — C's `strrchr` would match a file literally named `.gb`; std `extension()` does not. We follow std (a dotfile named `.gb` in a ROMs dir is not a ROM); cost: a pathological hidden file C would have shown. **R2** — C stored raw bytes in 256/512-byte buffers; Rust display names are `String` via `to_string_lossy` while the launch path stays a byte-exact `OsString` (C's `snprintf` truncation at 512 bytes is dropped; paths longer than 512 bytes now work).

- [ ] **Step 4: Run tests, watch them pass**

Run: `cargo --manifest-path engine/Cargo.toml test -p nativera_frontend roms`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/frontend/src/roms.rs
git commit -m "fe: roms scan module (case-sensitive .gb/.sh, 100-cap, placeholder, OsString paths)

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 3: Shell-free launch module

**Files:**
- Create: `engine/frontend/src/launch.rs` (replaces stub; unit tests inline)

**Interfaces:**
- Consumes: Task 2's `GameEntry.path: OsString`.
- Produces (used by Task 4):
  - `pub fn launch_game(run_sh: &std::path::Path, rom: &std::ffi::OsStr) -> Result<i32, LaunchError>` — spawns `run_sh` with a **single** argument `rom`, waits, and maps: normal exit → that code `as i32` (0–255); killed by signal → `128 + signum`; spawn or wait I/O failure → `Err`.
  - `#[derive(Debug, PartialEq)] pub enum LaunchError { Spawn(std::io::Error), Wait(std::io::Error) }`
  - plus `impl std::fmt::Display for LaunchError` — `"spawn failed: {inner}"` / `"wait failed: {inner}"` (Task 4's stderr line formats it)

- [ ] **Step 1: Write the failing tests**

Fixture in a `mktemp`-style dir (temp_dir + `process::id()`): write `run.sh` containing `#!/bin/sh\nprintf '%s' "$1" > "<capture>"\nexit 7`, chmod 0755. ROM path = `<dir>/game; rm -rf / "quoted'.gb` (the C test's injection payload, as `OsString`). Tests:
  - `injection_payload_arrives_byte_exact`: `launch_game(path("run.sh"), rom)` → `Ok(7)`; the capture file's bytes == `rom.as_os_str().to_string_lossy().as_bytes()` (byte-exact).
  - `missing_program_is_spawn_error`: `launch_game(path("/nonexistent/nativera-run.sh"), rom)` → `Err(LaunchError::Spawn(_))`.
  - `signal_kill_maps_to_128_plus_signum`: second script `kill -TERM $$` → `Ok(143)`.

- [ ] **Step 2: Run tests, watch them fail**

Run: `cargo --manifest-path engine/Cargo.toml test -p nativera_frontend launch`
Expected: FAIL — `launch_game`/`LaunchError` don't exist.

- [ ] **Step 3: Implement `launch_game`**

`std::process::Command::new(run_sh).arg(rom)` — no shell, `rom` is passed as one raw `OsString` argument. `.spawn()` → `Err` maps to `LaunchError::Spawn`. `.wait()` → `Err` maps to `LaunchError::Wait`; `Ok(status)` → `status.code().map(|c| c as i32).unwrap_or(128 + status.signal().unwrap_or(0))` (on this platform a non-exited status always carries a signal; the `unwrap_or(0)` makes the 128-fallback total).

Ruling: **R3** — the C contract says `-1` for both spawn *and* waitpid failures; the Rust `LaunchError` enum distinguishes them. Task 4 prints both to stderr; the only observable difference from C is the error text, not the control flow (loop continues in both).

- [ ] **Step 4: Run tests, watch them pass**

Run: `cargo --manifest-path engine/Cargo.toml test -p nativera_frontend launch`
Expected: all 3 PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/frontend/src/launch.rs
git commit -m "fe: shell-free launch module (raw OsString arg, 0-255 / 128+sig / Err contract)

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 4: UI layout, event loop, and main

**Files:**
- Create: `engine/frontend/src/ui.rs` (replaces stub), `engine/frontend/src/main.rs` (replaces Task 1's stub)

**Interfaces:**
- Consumes: Task 2 `roms::{scan, with_placeholder, GameEntry}`, Task 3 `launch::{launch_game, LaunchError}`.
- Produces:
  - `ui::run_frame(canvas: &mut impl sdl2::render::Canvas, games: &[GameEntry], selected: usize, title: &str, font: Option<&sdl2::ttf::Font>) -> Result<(), String>` — one full C-faithful frame: bg clear, header fill, optional title texture at `(40,25)`, per-row highlight (when `i == selected`) + row-text (selected white / unselected dim) at `(60, 120 + i*50)`, present.
  - `ui::handle_key(code: Option<sdl2::keyboard::Keycode>, selected: &mut usize, count: usize) -> Option<Action>` with `#[derive(Debug, PartialEq, Eq)] pub enum Action { MoveUp, MoveDown, Launch, Quit }` — `Up|W → MoveUp` (clamped), `Down|S → MoveDown` (clamped to `count-1`), `Return|Space → Launch`, `Escape → Quit`, anything else `None`.
  - `ui::constants` — the Global-Constraints values as `pub const`s (colors, rects, `ROW_Y0 = 120`, `ROW_STEP = 50`, `FRAME_DELAY_MS = 16`).

- [ ] **Step 1: Write the failing tests**

In `ui.rs` (unit) and `engine/frontend/tests/ui_smoke.rs` (integration, same `SDL_VIDEODRIVER=dummy` Once-pattern as Task 1):
  - `highlight_row_rects_match_c`: `highlight_rect(0)` == `Rect(40, 115, 1200, 40)`; `highlight_rect(3)` == `Rect(40, 265, 1200, 40)` (y = 120 + i*50 − 5); `row_y(2)` == 220; title dest == `(40, 25)`.
  - `key_mapping`: `handle_key(Some(Up), &mut 2, 5)` → `MoveUp`, `selected == 1`; `handle_key(Some(W), &mut 0, 5)` → `MoveUp` but `selected` stays `0`; `handle_key(Some(S), &mut 4, 5)` → `MoveDown`, stays `4`; `handle_key(Some(Space), ...)` → `Launch`; `handle_key(Some(Escape), ...)` → `Quit`; `handle_key(Some(A), ...)` → `None`.
  - `frame_renders_headless_with_font`: build the real 1280×720 window + accelerated/present_vsync canvas under the dummy driver, `ttf::init()`, `load_font` of the first existing DejaVu path — if neither path exists on this host, run the frame with `font = None` instead; call `run_frame` with a 3-entry fake game list and assert `Ok`.
  - `frame_renders_headless_without_font`: same, `font = None`, assert `Ok` (pins Review-Focus item 1).
  - `launch_error_prints_and_continues` (unit, no SDL): a helper `ui::report_launch(rom: &OsStr, err: &LaunchError)` returns the exact stderr line `"[fe] launch failed ({rom}): {err}\n"` — assert the returned string matches that shape; this is what `main` prints, so the Review-Focus-5 line is pinned without a display.

- [ ] **Step 2: Run tests, watch them fail**

Run: `cargo --manifest-path engine/Cargo.toml test -p nativera_frontend ui`
Expected: FAIL — `run_frame`/`handle_key`/constants don't exist.

- [ ] **Step 3: Implement `ui.rs`**

Constants from Global Constraints. `run_frame`: `canvas.set_draw_color(Color::RGB(24,26,33)); canvas.clear();` fill header rect `(0,0,1280,80)` in `(40,44,52)`; if `font` is `Some`: render `title` → `surface → texture` (the `font.render(text).blended(color)` → `texture_creator.create_texture_from_surface` pipeline) → `copy` to `(40,25)`; loop rows: highlight fill when selected (RGB(65,105,225) rect), row text in white/dim at `(60, row_y(i))`; `canvas.present()`. Texture/surface lifetimes follow the crate's own `ttf-demo` example. `handle_key` and `report_launch` per the Interfaces block.

- [ ] **Step 4: Implement `main.rs`**

`env::args().nth(1)` → `PathBuf` else `"./roms"`; `scan` + `with_placeholder`; `selected = 0`; SDL bootstrap exactly as Task 1's smoke test; font load = first `Ok` of the two DejaVu paths at 24pt, else `None`; loop: `poll_iter()` — `Event::Quit` → break; `Event::KeyDown { keycode, .. }` → `match handle_key` (Launch: if selected path non-empty, `launch_game(Path::new("./run.sh"), &entry.path)` → `Ok(code)`: `eprintln!("[fe] exit {code}")`; `Err(e)`: print `report_launch` line); then `run_frame(...)`; `thread::sleep(Duration::from_millis(16))`.

Ruling R4: the C `main` ignored `nativera_launch_game`'s `status` out-param entirely (launch.c only ever printed the spawn-failure line). The Rust `main` additionally prints the child's exit code, `[fe] exit {code}`, to stderr — deliberate: exit codes are the launch contract's signal channel, and printing them costs nothing. Control flow stays C-identical (always back to the menu).

- [ ] **Step 5: Run the full task suite**

Run: `cargo --manifest-path engine/Cargo.toml test -p nativera_frontend`
Expected: all tasks' tests PASS (roms, launch, ui, sdl_smoke).

- [ ] **Step 6: Commit**

```bash
git add engine/frontend/src/ui.rs engine/frontend/src/main.rs engine/frontend/tests/ui_smoke.rs
git commit -m "fe: C-faithful SDL event loop, frame render, and shell-free launch wiring

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 5: Makefile integration (build target only, C launcher stays)

**Files:**
- Modify: `Makefile` (add `fe` and `fe-tests` targets; `fe` to `.PHONY`; leave `all` and `tests` untouched)

**Interfaces:**
- Consumes: everything from Tasks 1–4 (crate + tests).
- Produces: `make fe` → release binary at `engine/target/release/nativera-fe` (already git-ignored via `engine/target/`); `make fe-tests` → runs the whole `nativera_frontend` suite.

- [ ] **Step 1: Add the targets**

```make
CARGO ?= cargo

fe:
	$(CARGO) --manifest-path engine/Cargo.toml build --release --bin nativera-fe

fe-tests:
	$(CARGO) --manifest-path engine/Cargo.toml test --release -p nativera_frontend
```

and add `fe fe-tests` to the `.PHONY` line.

- [ ] **Step 2: Verify both**

Run: `make fe && make fe-tests`
Expected: build succeeds; full test suite PASS (the C `make tests` suite still passes untouched — run `make tests` too).

- [ ] **Step 3: Commit**

```bash
git add Makefile
git commit -m "fe: make targets for the Rust front-end (fe, fe-tests); C launcher retained

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```
