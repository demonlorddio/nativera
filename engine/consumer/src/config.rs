//! The three-format config reader/writer (`json` / `ini` / `toml`) plus the
//! deep-merge and first-existing-file helpers. Port of the `load_cfg` /
//! `dump_cfg` / `merge_deep` block of `engine/recomp-launcher.py`.

use serde_json::Value;
use std::io::Write;
use std::path::Path;

use crate::manifest::FileRef;
use crate::placeholders::{guard_active, substitute, Placeholders};

/// The empty document every format returns for a missing / unreadable file.
pub fn empty() -> Value {
    Value::Object(serde_json::Map::new())
}

/// Recursively merge `override` into `base` (returns a new document). When
/// both sides are objects the keys are merged recursively; otherwise the
/// override value wins outright.
pub fn merge_deep(base: &Value, ovr: &Value) -> Value {
    match (base, ovr) {
        (Value::Object(b), Value::Object(o)) => {
            let mut m = b.clone();
            for (k, v) in o {
                m.insert(k.clone(), merge_deep(m.get(k).unwrap_or(&Value::Null), v));
            }
            Value::Object(m)
        }
        _ => ovr.clone(),
    }
}

/// A document counts as "empty" for the `new` discipline when it is `null`
/// or an empty object — i.e. the file is absent or blank.
pub(crate) fn is_empty_doc(v: &Value) -> bool {
    v.is_null() || matches!(v, Value::Object(m) if m.is_empty())
}

/// Render a config value the way Python's `str(v)` would for the shapes a
/// manifest can hold (strings, bools, numbers, null).
pub fn value_to_str(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Bool(b) => b.to_string(),
        Value::Number(n) => n.to_string(),
        Value::Null => String::new(),
        other => other.to_string(),
    }
}

/// Read a config document. A missing / empty / unparseable file yields the
/// empty document; json and ini parse errors are reported, a toml parse
/// error is swallowed (matching the Python reference's inner `try/except`).
pub fn load_cfg(path: &Path, fmt: &str) -> Value {
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(_) => return empty(),
    };
    if text.trim().is_empty() {
        return empty();
    }
    match fmt {
        "json" => match serde_json::from_str::<Value>(&text) {
            Ok(v) => v,
            Err(e) => {
                eprintln!("[recomp] config read failed ({path:?}): {e}");
                empty()
            }
        },
        "ini" => match ini::macro_safe_read(&text) {
            Ok(map) => {
                let mut obj = serde_json::Map::new();
                for (section, kv) in map {
                    let mut inner = serde_json::Map::new();
                    for (k, v) in kv {
                        inner.insert(k, Value::String(v.unwrap_or_default()));
                    }
                    obj.insert(section, Value::Object(inner));
                }
                Value::Object(obj)
            }
            Err(e) => {
                eprintln!("[recomp] config read failed ({path:?}): {e}");
                empty()
            }
        },
        "toml" => match toml::from_str::<Value>(&text) {
            Ok(v) => v,
            // Faithful to the reference: a bad toml doc is read as empty,
            // without a stderr note.
            Err(_) => empty(),
        },
        _ => empty(),
    }
}

/// Write `obj` to `path` in the requested format. The parent directory is
/// created as needed. The ini writer accumulates every pair under a single
/// `[recomp]` section (a nested value contributes its inner keys; a scalar
/// contributes `<key> = <value>`).
pub fn dump_cfg(obj: &Value, path: &Path, fmt: &str) -> std::io::Result<()> {
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        std::fs::create_dir_all(parent)?;
    }
    let text = match fmt {
        "json" => serde_json::to_string_pretty(obj).unwrap_or_default() + "\n",
        "toml" => toml::to_string(obj).unwrap_or_default(),
        "ini" => {
            let mut ini = ini::configparser::ini::Ini::new();
            if let Value::Object(map) = obj {
                for (k, v) in map {
                    match v {
                        Value::Object(inner) => {
                            for (a, b) in inner {
                                let s = value_to_str(b);
                                ini.setstr("recomp", a, Some(&s));
                            }
                        }
                        scalar => {
                            let s = value_to_str(scalar);
                            ini.setstr("recomp", k, Some(&s));
                        }
                    }
                }
            }
            ini.writes()
        }
        _ => String::new(),
    };
    let mut f = std::fs::File::create(path)?;
    f.write_all(text.as_bytes())
}

/// Walk a `config.file` reference and return the first entry that exists on
/// disk (guards respected). When no entry exists, fall back to the default
/// `{install}/config.json` target.
pub fn first_existing(file: &FileRef, ph: &Placeholders) -> String {
    let mut candidates: Vec<String> = Vec::new();
    for (p, g) in file.each_entry() {
        if p.trim().is_empty() {
            continue;
        }
        if let Some(cond) = g {
            if !guard_active(cond, ph) {
                continue;
            }
        }
        candidates.push(substitute(p, ph));
    }
    if candidates.is_empty() {
        // No usable entry: address the default config target instead.
        candidates.push(substitute("{install}/config.json", ph));
    }
    for c in &candidates {
        if Path::new(c).exists() {
            return c.clone();
        }
    }
    candidates.into_iter().next().unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn j(s: &str) -> Value {
        serde_json::from_str(s).unwrap()
    }

    #[test]
    fn merge_deep_recurse() {
        let base = j(r#"{"a":{"x":1},"b":2}"#);
        let ovr = j(r#"{"a":{"y":3},"c":4}"#);
        let out = merge_deep(&base, &ovr);
        assert_eq!(
            out,
            j(r#"{"a":{"x":1,"y":3},"b":2,"c":4}"#),
            "recursive merge keeps base keys, ovr wins on clash"
        );
    }

    #[test]
    fn merge_deep_scalar_wins() {
        let base = j(r#"{"a":{"x":1}}"#);
        let ovr = j(r#"{"a":5}"#);
        assert_eq!(merge_deep(&base, &ovr), j(r#"{"a":5}"#));
    }

    #[test]
    fn load_cfg_missing_is_empty() {
        assert_eq!(load_cfg(Path::new("/nonexistent/recomp-none.json"), "json"), empty());
    }

    #[test]
    fn first_existing_substitutes_and_walks() {
        let ph = crate::placeholders::build_placeholders(
            "/opt/t", "/disc/t", &HashMap::new(),
        );
        let root = std::env::temp_dir().join(format!("recomp-firstex-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let target = root.join("a.json");
        std::fs::write(&target, "{}\n").unwrap();
        let target_s = target.to_string_lossy().into_owned();
        let ref_ok: FileRef =
            serde_json::from_str(&format!(r#"[{{"path": "{target_s}"}}, "b"]"#)).unwrap();
        assert_eq!(first_existing(&ref_ok, &ph), target_s);
        // No candidate exists (guardless "b" is relative, not substituted
        // against anything here) → falls back to the first candidate.
        let ref_miss: FileRef = serde_json::from_str(r#"["nope"]"#).unwrap();
        assert_eq!(first_existing(&ref_miss, &ph), "nope");
        let _ = std::fs::remove_dir_all(&root);
    }
}
