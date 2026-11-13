//! The RecompLauncher v1 manifest consumer: `verify → prepare → config →
//! start → saves`, each phase individually callable. Rust port of
//! `engine/recomp-launcher.py` (kept as the on-target reference until this
//! one is proven in-tree).
//!
//! The public surface is headless: build a [`Launcher`] against a title
//! install dir, run the phases, inspect the [`Exit`] codes. The CLI in
//! `main.rs` is a thin shell over it.

pub mod config;
pub mod manifest;
pub mod placeholders;

pub use manifest::{Check, ConfigEntry, FileRef, Game, Manifest, Prepare, RefEntry, Start};
pub use placeholders::{build_placeholders, guard_active, sanitize_env, substitute, Placeholders};

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashMap;
use std::io::Write;

/// The `RECOMPLAUNCHER_VERSION` injected on every launch (matches the
/// Python reference, which hard-codes `"nativera-dev"`).
pub const RECOMP_VERSION: &str = "nativera-dev";

/// A single save target larger than this is skipped (Python: 64 MB).
const SAVES_CAP: u64 = 64 * 1024 * 1024;

/// A phase exit code the CLI converts to the process exit status.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Exit(pub i32);

/// Errors that happen before any phase can run (bad install dir / manifest).
#[derive(Debug)]
pub enum LaunchError {
    /// No `recomplauncher.json` under the title dir.
    MissingManifest(String),
    /// The manifest is present but is not a valid format:1 document.
    BadManifest(String),
}

impl std::fmt::Display for LaunchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::MissingManifest(p) => write!(f, "no manifest at {p} (expected recomplauncher.json)"),
            Self::BadManifest(e) => write!(f, "bad manifest: {e}"),
        }
    }
}

impl std::error::Error for LaunchError {}

/// `state/prepare-state.json` — the stamp `prepare` writes/compares.
#[derive(Debug, Serialize, Deserialize)]
struct PrepareStamp {
    #[serde(default)]
    game: Option<String>,
    #[serde(default)]
    mtime: Option<f64>,
    #[serde(default)]
    size: Option<u64>,
    #[serde(default)]
    at: Option<f64>,
}

pub struct Launcher {
    pub install: String,
    /// "" = unset (no `--game` and no `NATIVERA_GAME`).
    pub game: String,
    /// Process env + `--env` overrides; handed to every child.
    pub env: HashMap<String, String>,
    pub manifest: Manifest,
    pub ph: Placeholders,
    out: Box<dyn Write>,
}

impl Launcher {
    /// Point the log stream somewhere else (tests, embedded use).
    pub fn set_out(&mut self, out: impl Write + 'static) {
        self.out = Box::new(out);
    }

    /// Build a consumer for `title`.
    ///
    /// Rulings vs. the Python reference:
    /// - `title` is canonicalized; when it names a *file* (e.g. the manifest
    ///   itself) its parent dir is used (Python's `abspath` didn't).
    /// - The placeholder map is built from the process env *before*
    ///   `overrides` are applied — the same order the Python `main` had
    ///   (`--env` lands on `self.env` after `build_placeholders`).
    pub fn new(
        title: &str,
        game_arg: Option<&str>,
        overrides: &[(String, String)],
    ) -> Result<Self, LaunchError> {
        let resolved =
            std::fs::canonicalize(title).map_err(|_| LaunchError::MissingManifest(title.to_string()))?;
        let install_path = if resolved.is_file() {
            resolved.parent().map(|p| p.to_path_buf()).unwrap_or(resolved)
        } else {
            resolved
        };
        let install = install_path.to_string_lossy().into_owned();
        let manifest_path = format!("{install}/recomplauncher.json");
        let text = std::fs::read_to_string(&manifest_path)
            .map_err(|_| LaunchError::MissingManifest(manifest_path.clone()))?;
        let manifest: Manifest =
            serde_json::from_str(&text).map_err(|e| LaunchError::BadManifest(e.to_string()))?;

        let mut env: HashMap<String, String> = std::env::vars().collect();
        let game = game_arg
            .map(String::from)
            .or_else(|| env.get("NATIVERA_GAME").cloned())
            .unwrap_or_default();
        let ph = build_placeholders(&install, &game, &env);
        for (k, v) in overrides {
            env.insert(k.clone(), v.clone());
        }
        Ok(Launcher {
            install,
            game,
            env,
            manifest,
            ph,
            out: Box::new(std::io::stdout()),
        })
    }

    fn state_dir(&self) -> String {
        format!("{}/.recomp", self.install)
    }

    fn line(&mut self, msg: impl std::fmt::Display) {
        let _ = writeln!(self.out, "{msg}");
        let _ = self.out.flush();
    }

    /// Python `_fail`: one shared failure path — note it always logs the
    /// `[verify] MISSING` prefix, even when called from `prepare`.
    fn fail(&mut self, code: i32, label: Option<&str>) -> Result<(), Exit> {
        let label = label.unwrap_or("BYO game copy");
        self.line(format!("[verify] MISSING — {label}"));
        if let Some(msg) = self.manifest.exit_codes.get(&code.to_string()) {
            self.line(format!("  → {msg}"));
        }
        Err(Exit(code))
    }

    // 1) verify the user's BYO copy ------------------------------------------------
    pub fn verify(&mut self) -> Result<(), Exit> {
        let label = self.manifest.game.label.clone();
        let check = self.manifest.game.check.clone();
        if self.game.is_empty() {
            return self.fail(2, label.as_deref());
        }
        let game_sub = substitute(&self.game, &self.ph);
        for rel in [
            check.file.as_deref(),
            check.iso9660.as_deref(),
            check.xdvdfs.as_deref(),
        ]
        .into_iter()
        .flatten()
        {
            let target = std::path::PathBuf::from(&game_sub).join(substitute(rel, &self.ph));
            if !target.exists() {
                return self.fail(2, label.as_deref());
            }
            // Python `size or size_bytes`: 0 is falsy, so a 0 size falls
            // through to size_bytes.
            let want = check
                .size
                .filter(|s| *s > 0)
                .or_else(|| check.size_bytes.filter(|s| *s > 0));
            if let Some(w) = want {
                if let Ok(md) = std::fs::metadata(&target) {
                    if md.is_file() && md.len() as i64 != w {
                        return self.fail(2, label.as_deref());
                    }
                }
            }
        }
        self.line(format!("[verify] OK — game copy at {}", self.game));
        Ok(())
    }

    /// A program entry is a path or a bare name; resolve to an exec target
    /// (Python `_resolve_exe`, verbatim).
    fn resolve_exe(&self, raw: &str) -> String {
        let p = substitute(raw, &self.ph);
        let install = self.ph.get("install").unwrap_or(&self.install).to_string();
        if p.starts_with('/') && std::path::Path::new(&p).exists() {
            return p;
        }
        let base = p.rsplit('/').next().unwrap_or(&p);
        let cand = format!("{install}/{base}");
        if std::path::Path::new(&cand).exists() {
            return cand;
        }
        which(&p).unwrap_or(cand)
    }

    // 2) one-time native build / cache ---------------------------------------------
    pub fn prepare(&mut self, force: bool) -> Result<(), Exit> {
        let Some(pre) = self.manifest.prepare.clone() else {
            return Ok(());
        };
        let prog = self.resolve_exe(pre.program.first().as_str());
        let stamp_path = std::path::PathBuf::from(self.state_dir()).join("prepare-state.json");
        if !force && stamp_path.exists() && self.stamp_matches(&stamp_path) {
            let label = pre.label.as_deref().unwrap_or("native build");
            self.line(format!("[prepare] up to date — skipping ({label})"));
            return Ok(());
        }
        let label_run = pre.label.as_deref().unwrap_or("building native module");
        if stamp_path.exists() {
            self.line(format!("[prepare] {label_run} …"));
        } else {
            self.line(format!("[prepare] {label_run} (first run) …"));
        }
        // Faithful: prepare args are NOT empty-filtered (unlike start args)
        // and the child inherits our cwd (unlike start, which chdirs).
        let args: Vec<String> = pre.args.iter().map(|a| substitute(a, &self.ph)).collect();
        let mut child_env = self.env.clone();
        child_env.insert("RECOMPLAUNCHER".into(), "1".into());
        let mut cmd = std::process::Command::new(&prog);
        cmd.args(&args).env_clear().envs(&child_env);
        let mut child = match cmd.spawn() {
            Ok(c) => c,
            Err(e) => {
                self.line(format!("[prepare] couldn't run {prog}: {e}"));
                return Err(Exit(3));
            }
        };
        let deadline = pre
            .timeout
            .map(|t| std::time::Instant::now() + std::time::Duration::from_secs_f64(t));
        loop {
            match child.try_wait() {
                Ok(Some(status)) => {
                    if !status.success() {
                        // Faithful quirk: a failed build goes through
                        // `_fail(3, None)` — the `[verify] MISSING` prefix
                        // plus exit_codes["3"] when the manifest has one.
                        return self.fail(3, None);
                    }
                    let _ = std::fs::create_dir_all(&self.state_dir());
                    let stamp = PrepareStamp {
                        game: if self.game.is_empty() {
                            None
                        } else {
                            Some(self.game.clone())
                        },
                        mtime: Some(current_mtime(&self.game)),
                        size: game_size(&self.game),
                        at: Some(now_secs()),
                    };
                    if let Err(e) = std::fs::write(
                        &stamp_path,
                        serde_json::to_string(&stamp).unwrap_or_default(),
                    ) {
                        self.line(format!("[prepare] stamp write failed: {e}"));
                    }
                    self.line("[prepare] done");
                    return Ok(());
                }
                Ok(None) => {
                    if let Some(dl) = deadline {
                        if std::time::Instant::now() >= dl {
                            let _ = child.kill();
                            let _ = child.wait();
                            self.line("[prepare] timed out");
                            return Err(Exit(3));
                        }
                    }
                    std::thread::sleep(std::time::Duration::from_millis(50));
                }
                Err(e) => {
                    self.line(format!("[prepare] wait failed: {e}"));
                    return Err(Exit(3));
                }
            }
        }
    }

    /// Python `_stamp_matches`: the stamp's game + mtime (±1 s) must still
    /// match the player's copy.
    fn stamp_matches(&self, path: &std::path::Path) -> bool {
        let text = match std::fs::read_to_string(path) {
            Ok(t) => t,
            Err(_) => return false,
        };
        let s: PrepareStamp = match serde_json::from_str(&text) {
            Ok(v) => v,
            Err(_) => return false,
        };
        if s.game.as_deref().unwrap_or("") != self.game {
            return false;
        }
        let cur = current_mtime(&self.game);
        match s.mtime {
            Some(st) => (cur - st).abs() < 1.0,
            None => false,
        }
    }

    // 3) write settings --------------------------------------------------------------
    pub fn config(&mut self) -> Result<(), Exit> {
        let entries = self.manifest.config.clone();
        for entry in entries {
            let fmt = entry.format.clone();
            let target = config::first_existing(&entry.file, &self.ph);
            let cur = config::load_cfg(std::path::Path::new(&target), &fmt);
            let mut base = cur.clone();
            // `new` only when the file is absent / empty. An explicit
            // `"new": null` is treated as absent (Python would merge None —
            // a pathological input, ruled out).
            if let Some(new_v) = &entry.new {
                if config::is_empty_doc(&cur) {
                    base = config::merge_deep(&base, new_v);
                }
            }
            // `ensure`: fill missing top-level keys, keep everything else.
            if let (Some(ensure), Value::Object(b)) = (&entry.ensure, &mut base) {
                if let Value::Object(m) = ensure {
                    for (k, v) in m {
                        b.entry(k.clone()).or_insert(v.clone());
                    }
                }
            }
            // `always`: overwrite unconditionally (highest precedence).
            if let Some(always) = &entry.always {
                base = config::merge_deep(&base, always);
            }
            if base != cur {
                match config::dump_cfg(&base, std::path::Path::new(&target), &fmt) {
                    Ok(()) => {
                        let name = std::path::Path::new(&target)
                            .file_name()
                            .map(|s| s.to_string_lossy().into_owned())
                            .unwrap_or_else(|| target.clone());
                        self.line(format!("[config] wrote {name}"));
                    }
                    Err(e) => self.line(format!("[config] write failed ({target}): {e}")),
                }
            }
        }
        Ok(())
    }

    // 4) launch ----------------------------------------------------------------------
    pub fn start(&mut self) -> Result<(), Exit> {
        let st = self.manifest.start.clone();
        let target = self.resolve_exe(st.program.first().as_str());
        // Omit any arg that resolves to empty (an absent {input:<id>}).
        let args: Vec<String> = st
            .args
            .iter()
            .map(|a| substitute(a, &self.ph))
            .filter(|a| !a.is_empty())
            .collect();
        let mut envs = self.env.clone();
        if !st.env.is_empty() {
            // Faithful: Python's dict-substitute touches VALUES only; keys
            // pass through verbatim.
            let mut manifest_env: HashMap<String, String> = HashMap::new();
            for (k, v) in &st.env {
                manifest_env.insert(k.clone(), substitute(v, &self.ph));
            }
            for (k, v) in sanitize_env(&manifest_env) {
                envs.insert(k, v);
            }
        }
        // The standard variables RecompLauncher injects on every launch.
        envs.entry("RECOMPLAUNCHER".into()).or_insert("1".into());
        envs
            .entry("RECOMPLAUNCHER_VERSION".into())
            .or_insert(RECOMP_VERSION.into());
        envs.entry("RECOMPLAUNCHER_MODE".into()).or_insert("bigpicture".into());
        envs.insert("RECOMPLAUNCHER_PAD".into(), self.ph.pad().to_string());
        envs.insert("RECOMPLAUNCHER_INSTALL".into(), self.install.clone());
        if !self.game.is_empty() {
            envs.insert("RECOMPLAUNCHER_GAME".into(), self.game.clone());
        }
        let base = std::path::Path::new(&target)
            .file_name()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| target.clone());
        self.line(format!("[start] exec {base} {}", args.join(" ")));
        let mut cmd = std::process::Command::new(&target);
        cmd.args(&args)
            .current_dir(&self.install)
            .env_clear()
            .envs(&envs);
        let status = match cmd.status() {
            Ok(s) => s,
            // Python catches FileNotFoundError; other spawn failures get the
            // same line (with the OS error appended — a ruling, it's more
            // useful than a silent exit 3).
            Err(e) => {
                self.line(format!("[start] binary not found: {target} ({e})"));
                return Err(Exit(3));
            }
        };
        let rc = status.code().unwrap_or(-1);
        // Faithful structure: start() collects saves and reports the exit
        // code before returning — one method does all three.
        self.saves_after();
        self.report_exit(rc);
        Ok(())
    }

    // 5) collect user data ------------------------------------------------------------
    pub fn saves_after(&mut self) {
        let pats = self.manifest.saves.clone();
        let save_root = format!("{}/saves", self.state_dir());
        if let Err(e) = std::fs::create_dir_all(&save_root) {
            self.line(format!("[saves] cannot create {save_root}: {e}"));
        }
        let mut kept = 0usize;
        for (i, pat) in pats.iter().enumerate() {
            let p = substitute(pat, &self.ph);
            let mut matches: Vec<std::path::PathBuf> =
                glob::glob(&p).into_iter().flatten().filter_map(|r| r.ok()).collect();
            matches.sort();
            if matches.is_empty() && std::path::Path::new(&p).exists() {
                matches.push(std::path::PathBuf::from(&p));
            }
            for m in matches {
                if !m.exists() {
                    continue;
                }
                let mstr = m.to_string_lossy().into_owned();
                let trimmed = mstr.trim_end_matches('/');
                let base = trimmed
                    .rsplit('/')
                    .next()
                    .filter(|s| !s.is_empty())
                    .unwrap_or("root");
                let dest = std::path::Path::new(&save_root).join(format!("{i}_{base}"));
                // Python uses shutil.copy2 (mtime-preserving); fs::copy is
                // the closest std primitive — acceptable on the distro.
                let res = if m.is_dir() {
                    copy_dir(&m, &dest)
                } else {
                    match std::fs::metadata(&m) {
                        Ok(md) if md.len() > SAVES_CAP => {
                            self.line(format!("[saves] skip {base} (>64 MB)"));
                            continue;
                        }
                        _ => std::fs::copy(&m, &dest).map(|_| ()),
                    }
                };
                match res {
                    Ok(_) => kept += 1,
                    Err(e) => self.line(format!("[saves] skip {mstr}: {e}")),
                }
            }
        }
        self.line(format!(
            "[saves] kept {kept} of {} user-data target(s) under {save_root}",
            pats.len()
        ));
    }

    /// Python `_report_exit`.
    pub fn report_exit(&mut self, rc: i32) {
        if rc == 0 {
            return;
        }
        let key = rc.to_string();
        if let Some(msg) = self.manifest.exit_codes.get(&key) {
            self.line(format!("[exit {rc}] {msg}"));
        } else if rc == 130 || rc == 137 {
            self.line(format!("[exit {rc}] closed by the user"));
        } else {
            self.line(format!(
                "[exit {rc}] the program stopped unexpectedly — see the Logs folder"
            ));
        }
    }

    // full sequence ------------------------------------------------------------------
    pub fn run(&mut self, force_prepare: bool) -> Result<(), Exit> {
        self.verify()?;
        self.prepare(force_prepare)?;
        self.config()?;
        self.start()
    }
}

/// Python `shutil.which` for a bare name.
fn which(name: &str) -> Option<String> {
    if name.is_empty() || name.contains('/') {
        return None;
    }
    let path = std::env::var("PATH").unwrap_or_default();
    for dir in path.split(':') {
        if dir.is_empty() {
            continue;
        }
        let cand = format!("{dir}/{name}");
        if is_exec(&cand) {
            return Some(cand);
        }
    }
    None
}

#[cfg(unix)]
fn is_exec(p: &str) -> bool {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(p)
        .map(|m| m.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}

#[cfg(not(unix))]
fn is_exec(p: &str) -> bool {
    std::path::Path::new(p).is_file()
}

/// `os.path.getmtime` as f64; 0.0 when the path is gone (Python's
/// `getmtime(path) if exists else 0`).
#[cfg(unix)]
fn current_mtime(p: &str) -> f64 {
    use std::os::unix::fs::MetadataExt;
    std::fs::metadata(p).map(|m| m.mtime() as f64).unwrap_or(0.0)
}

#[cfg(not(unix))]
fn current_mtime(_p: &str) -> f64 {
    0.0
}

fn game_size(p: &str) -> Option<u64> {
    std::fs::metadata(p).ok().filter(|m| m.is_file()).map(|m| m.len())
}

fn now_secs() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs_f64())
        .unwrap_or(0.0)
}

/// `shutil.copytree(m, dest, dirs_exist_ok=True)` — merge into an existing
/// dir, recurse.
fn copy_dir(src: &std::path::Path, dst: &std::path::Path) -> std::io::Result<()> {
    std::fs::create_dir_all(dst)?;
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let t = entry.path();
        if t.is_dir() {
            copy_dir(&t, &dst.join(entry.file_name()))?;
        } else {
            std::fs::copy(&t, &dst.join(entry.file_name()))?;
        }
    }
    Ok(())
}
