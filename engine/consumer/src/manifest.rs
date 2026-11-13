//! serde types for the RecompLauncher format:1 manifest (`recomplauncher.json`).
//!
//! The manifest is DATA, not code: the headless consumer drives
//! `game` / `start` / `prepare` / `config` / `saves` / `exit_codes`. The
//! UI/store fields (`settings`, `looks`, `mods`, `inputs`, `folders`,
//! `actions`, `games`) are kept opaque, and unknown keys (`$schema`, …) are
//! ignored.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

/// One file entry: a bare path string, or `{ "path": ..., "if": ... }`.
#[derive(Debug, Clone, PartialEq, Deserialize, Serialize)]
#[serde(untagged)]
pub enum RefEntry {
    Path(String),
    Guarded {
        path: String,
        #[serde(rename = "if", default)]
        if_cond: Option<String>,
    },
}

impl RefEntry {
    pub fn path(&self) -> &str {
        match self {
            RefEntry::Path(p) => p,
            RefEntry::Guarded { path, .. } => path,
        }
    }

    pub fn if_cond(&self) -> Option<&str> {
        match self {
            RefEntry::Path(_) => None,
            RefEntry::Guarded { if_cond, .. } => if_cond.as_deref(),
        }
    }
}

/// A file reference: string | single `{path,if}` object | list of them.
///
/// `start.program` / `prepare.program` resolve the **first** entry only (as
/// the Python reference does with `program[0]`); `config.file` walks all
/// entries and takes the first that exists.
#[derive(Debug, Clone, PartialEq, Deserialize, Serialize)]
#[serde(untagged)]
pub enum FileRef {
    Path(String),
    Single(RefEntry),
    List(Vec<RefEntry>),
}

impl Default for FileRef {
    fn default() -> Self {
        FileRef::Path(String::new())
    }
}

impl FileRef {
    /// All entries in order: `(path, optional guard)`.
    pub fn each_entry(&self) -> Vec<(&str, Option<&str>)> {
        match self {
            FileRef::Path(p) => vec![(p.as_str(), None)],
            FileRef::Single(e) => vec![(e.path(), e.if_cond())],
            FileRef::List(items) => items.iter().map(|e| (e.path(), e.if_cond())).collect(),
        }
    }

    /// The first entry's path (or "" when the reference is empty).
    pub fn first(&self) -> String {
        self.each_entry()
            .into_iter()
            .next()
            .map(|(p, _)| p.to_string())
            .unwrap_or_default()
    }
}

/// The `game` block: what the player drops in + how to confirm it.
#[derive(Debug, Clone, Default, Deserialize, Serialize)]
pub struct Game {
    pub kind: Option<String>,
    pub label: Option<String>,
    #[serde(default)]
    pub check: Check,
    /// `dumps[]` (Redump CRCs) is informational; the consumer never gates on it.
    pub dumps: Option<Value>,
}

/// One of `file` / `iso9660` / `xdvdfs` → the path matched inside the
/// player's own copy; optional `size` / `size_bytes` on the named dump.
#[derive(Debug, Clone, Default, Deserialize, Serialize)]
pub struct Check {
    pub file: Option<String>,
    pub iso9660: Option<String>,
    pub xdvdfs: Option<String>,
    pub size: Option<i64>,
    pub size_bytes: Option<i64>,
}

/// `start`: how to launch. `program` is required (a format-1 manifest
/// without it is rejected at parse time).
#[derive(Debug, Clone, Deserialize)]
pub struct Start {
    pub program: FileRef,
    #[serde(default)]
    pub args: Vec<String>,
    /// Launch env — sanitized against `placeholders::BLOCKED_ENV`.
    #[serde(default)]
    pub env: BTreeMap<String, String>,
}

impl Default for Start {
    fn default() -> Self {
        Start {
            program: FileRef::default(),
            args: Vec::new(),
            env: BTreeMap::new(),
        }
    }
}

/// `prepare`: one-time native build/cache. `timeout` is in seconds (or
/// absent → wait forever).
#[derive(Debug, Clone, Deserialize)]
pub struct Prepare {
    pub program: FileRef,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default)]
    pub label: Option<String>,
    #[serde(default)]
    pub timeout: Option<f64>,
}

/// `config[]`: one settings write plan.
#[derive(Debug, Clone, Deserialize)]
pub struct ConfigEntry {
    #[serde(default = "ConfigEntry::default_id")]
    pub id: String,
    #[serde(default = "ConfigEntry::default_fmt")]
    pub format: String,
    #[serde(default)]
    pub file: FileRef,
    #[serde(default)]
    pub game_path: Option<String>,
    /// Write disciplines, lowest precedence first: `new` (only when the file
    /// is absent/empty), `ensure` (fill missing top-level keys),
    /// `always` (overwrite every launch).
    #[serde(default)]
    pub new: Option<Value>,
    #[serde(default)]
    pub ensure: Option<Value>,
    #[serde(default)]
    pub always: Option<Value>,
}

impl Default for ConfigEntry {
    fn default() -> Self {
        ConfigEntry {
            id: Self::default_id(),
            format: Self::default_fmt(),
            file: FileRef::default(),
            game_path: None,
            new: None,
            ensure: None,
            always: None,
        }
    }
}

impl ConfigEntry {
    fn default_id() -> String {
        "main".into()
    }
    fn default_fmt() -> String {
        "json".into()
    }
}

/// The top-level manifest. Unknown keys are ignored; every field defaults so
/// a minimal manifest (game + start) parses.
#[derive(Debug, Clone, Deserialize, Default)]
#[serde(default)]
pub struct Manifest {
    pub format: Option<i64>,
    pub name: Option<String>,
    /// GitHub `owner/name` supplying install/update — not yet driven in-tree
    /// (the distro ships the binary), kept for the front end.
    pub repo: Option<String>,
    pub game: Game,
    pub start: Start,
    pub prepare: Option<Prepare>,
    pub config: Vec<ConfigEntry>,
    /// User-data globs collected after each session.
    pub saves: Vec<String>,
    /// exit-code → player-facing string.
    pub exit_codes: BTreeMap<String, String>,
    // UI/store fields — opaque to the headless consumer:
    pub games: Option<Value>,
    pub settings: Option<Value>,
    pub mods: Option<Value>,
    pub folders: Option<Value>,
    pub actions: Option<Value>,
    pub inputs: Option<Value>,
    pub looks: Option<Value>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fileref_forms() {
        let m: FileRef = serde_json::from_str(r#""asuraswrath""#).unwrap();
        assert_eq!(m.first(), "asuraswrath");

        let m: FileRef =
            serde_json::from_str(r#"{"path": "a.cfg", "if": "ON"}"#).unwrap();
        assert_eq!(m.first(), "a.cfg");
        let e = m.each_entry();
        assert_eq!(e.len(), 1);
        assert_eq!(e[0].1, Some("ON"));

        let m: FileRef =
            serde_json::from_str(r#"["a", {"path": "b", "if": "ON"}]"#).unwrap();
        let e = m.each_entry();
        assert_eq!(e.len(), 2);
        assert_eq!(m.first(), "a");
        assert_eq!(e[1].1, Some("ON"));
    }

    #[test]
    fn manifest_parses_minimal_shape() {
        let m: Manifest = serde_json::from_str(
            r#"{"format":1,"game":{"check":{"file":"PS3_GAME/PARAM.SFO"}},
                 "start":{"program":["x"],"args":["--game","{game}"]}}"#,
        )
        .unwrap();
        assert_eq!(m.format, Some(1));
        assert_eq!(m.start.program.first(), "x");
        assert!(m.prepare.is_none());
        assert!(m.saves.is_empty());
        assert_eq!(m.game.check.file.as_deref(), Some("PS3_GAME/PARAM.SFO"));
    }

    #[test]
    fn config_entry_defaults() {
        let e: ConfigEntry = serde_json::from_str("{}").unwrap();
        assert_eq!(e.id, "main");
        assert_eq!(e.format, "json");
        assert_eq!(e.file.first(), "");
    }
}
