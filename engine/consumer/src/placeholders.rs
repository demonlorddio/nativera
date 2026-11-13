//! Placeholder expansion, the env blocklist, and the per-launch placeholder
//! map. Port of the `build_placeholders` / `substitute` / `_BLOCKED_ENV`
//! section of `engine/recomp-launcher.py`.

use std::collections::{BTreeMap, HashMap};

/// System env names a manifest `start.env` may never set — RecompLauncher
/// blocks these so a manifest can't redirect the dynamic loader or the shell.
pub const BLOCKED_ENV: &[&str] = &[
    "PATH", "LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", "LD_ORIGIN_PATH",
    "LD_DEBUG", "LD_BIND_NOW", "LD_BIND_NOLOAD", "LD_USE_LOAD_BIAS",
    "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH", "DYLD_FORCE_NODELETE",
    "NODE_OPTIONS", "PYTHONPATH", "PYTHONSTARTUP", "BASH_ENV",
    "HOME", "USER", "SHELL",
];

pub fn is_blocked(name: &str) -> bool {
    BLOCKED_ENV.iter().any(|b| *b == name)
}

/// Strip the env keys a manifest may not override.
pub fn sanitize_env(raw: &HashMap<String, String>) -> HashMap<String, String> {
    raw.iter()
        .filter(|(k, _)| !is_blocked(k))
        .map(|(k, v)| (k.clone(), v.clone()))
        .collect()
}

/// The v1 placeholder set for one launch.
#[derive(Debug, Clone, Default)]
pub struct Placeholders {
    map: BTreeMap<String, String>,
}

impl Placeholders {
    pub fn get(&self, key: &str) -> Option<&str> {
        self.map.get(key).map(|s| s.as_str())
    }

    /// `{pad}` — a controller *type* (xbox/playstation), not an index.
    pub fn pad(&self) -> &str {
        self.map.get("pad").map(|s| s.as_str()).unwrap_or("xbox")
    }
}

/// Python `_xdg`: `env[var]` when non-empty, else the tilde-expanded default.
fn xdg(env: &HashMap<String, String>, var: &str, default: &str) -> String {
    match env.get(var) {
        Some(v) if !v.is_empty() => v.clone(),
        _ => expand_user(default, env.get("HOME").map(|s| s.as_str())),
    }
}

/// `os.path.expanduser`: a leading `~` resolves against $HOME; when HOME is
/// unknown the string is left as-is (Python's expanduser does the same).
fn expand_user(s: &str, home: Option<&str>) -> String {
    if let Some(rest) = s.strip_prefix("~/") {
        home.map(|h| format!("{h}/{rest}"))
            .unwrap_or_else(|| s.to_string())
    } else if s == "~" {
        home.map(String::from).unwrap_or_else(|| s.to_string())
    } else {
        s.to_string()
    }
}

pub fn build_placeholders(
    install: &str,
    game: &str,
    env: &HashMap<String, String>,
) -> Placeholders {
    let home = xdg(env, "HOME", "~");
    Placeholders {
        map: BTreeMap::from([
            ("install".into(), install.to_string()),
            ("game".into(), game.to_string()),
            ("home".into(), home.clone()),
            ("config".into(), xdg(env, "XDG_CONFIG_HOME", "~/.config")),
            ("local".into(), xdg(env, "XDG_DATA_HOME", "~/.local/share")),
            ("documents".into(), xdg(env, "NATIVERA_DOCUMENTS", &home)),
            ("pad".into(), env.get("NATIVERA_PAD").cloned().unwrap_or_else(|| "xbox".into())),
        ]),
    }
}

/// Resolve `{placeholder}` tokens in `input`.
///
/// `{input:<id>}` → `NATIVERA_INPUT_<ID>` (id uppercased) — "" when the
/// optional input is absent, so the empty arg is dropped by `start`.
/// Unknown tokens are left literal so a manifest can still name them.
pub fn substitute(input: &str, ph: &Placeholders) -> String {
    let chars: Vec<char> = input.chars().collect();
    let mut out = String::with_capacity(input.len());
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '{' {
            if let Some((tok, j)) = parse_token(&chars, i + 1) {
                if let Some(repl) = resolve_token(&tok, ph) {
                    out.push_str(&repl);
                } else {
                    out.push('{');
                    out.push_str(&tok);
                    out.push('}');
                }
                i = j;
                continue;
            }
        }
        out.push(chars[i]);
        i += 1;
    }
    out
}

/// The Python regex `\{([a-z_]+(?::[a-z0-9_]+)?)\}` as a hand-rolled
/// scanner: name = `[a-z_]+`, optional arg = `:` + `[a-z0-9_]+`, then a
/// closing `}`. Returns `(token, next_index)`; anything else is not a
/// token (the braces stay literal).
fn parse_token(chars: &[char], start: usize) -> Option<(String, usize)> {
    let mut name = String::new();
    let mut j = start;
    while j < chars.len() && (chars[j].is_ascii_lowercase() || chars[j] == '_') {
        name.push(chars[j]);
        j += 1;
    }
    if name.is_empty() {
        return None;
    }
    let mut arg: Option<String> = None;
    if j < chars.len() && chars[j] == ':' {
        let mut a = String::new();
        let mut k = j + 1;
        while k < chars.len()
            && (chars[k].is_ascii_lowercase()
                || chars[k].is_ascii_digit()
                || chars[k] == '_')
        {
            a.push(chars[k]);
            k += 1;
        }
        if a.is_empty() {
            return None; // a `:` with no arg is not a token
        }
        arg = Some(a);
        j = k;
    }
    if j >= chars.len() || chars[j] != '}' {
        return None;
    }
    Some((
        match arg {
            Some(a) => format!("{name}:{a}"),
            None => name,
        },
        j + 1,
    ))
}

fn resolve_token(tok: &str, ph: &Placeholders) -> Option<String> {
    if let Some(id) = tok.strip_prefix("input:") {
        Some(
            std::env::var(format!("NATIVERA_INPUT_{}", id.to_uppercase()))
                .unwrap_or_default(),
        )
    } else {
        ph.get(tok).map(String::from)
    }
}

/// An `if` guard on a file entry (LAUNCHER.md): substitute, trim,
/// lowercase; the entry is active unless it lands in
/// `{"", "0", "false", "none", "null"}`.
pub fn guard_active(cond: &str, ph: &Placeholders) -> bool {
    let v = substitute(cond, ph).trim().to_ascii_lowercase();
    !matches!(v.as_str(), "" | "0" | "false" | "none" | "null")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ph_for(home: Option<&str>, cfg: Option<&str>) -> Placeholders {
        let mut env: HashMap<String, String> = HashMap::new();
        if let Some(h) = home {
            env.insert("HOME".into(), h.into());
        }
        if let Some(c) = cfg {
            env.insert("XDG_CONFIG_HOME".into(), c.into());
        }
        build_placeholders("/opt/asura", "/disc/asura", &env)
    }

    #[test]
    fn known_tokens_resolve() {
        let ph = ph_for(Some("/home/u"), None);
        assert_eq!(
            substitute("{install}/settings.json", &ph),
            "/opt/asura/settings.json"
        );
        assert_eq!(substitute("{game}/PS3_GAME", &ph), "/disc/asura/PS3_GAME");
        assert_eq!(ph.pad(), "xbox");
    }

    #[test]
    fn unknown_tokens_stay_literal() {
        let ph = ph_for(None, None);
        assert_eq!(substitute("{bogus}", &ph), "{bogus}");
        // `5` is not in [a-z_], so this is not a token.
        assert_eq!(substitute("100{5}p", &ph), "100{5}p");
        assert_eq!(substitute("plain text", &ph), "plain text");
    }

    #[test]
    fn absent_input_resolves_to_empty() {
        let ph = ph_for(None, None);
        // NATIVERA_INPUT_ZZ_NATIVERA_TEST_QQ is not set in the test env.
        assert_eq!(
            substitute("--dlss {input:zz_nativera_test_qq}", &ph),
            "--dlss "
        );
    }

    #[test]
    fn xdg_defaults_expand_against_home() {
        let ph = ph_for(Some("/home/u"), None);
        assert_eq!(ph.get("home"), Some("/home/u"));
        assert_eq!(ph.get("config"), Some("/home/u/.config"));
        assert_eq!(ph.get("local"), Some("/home/u/.local/share"));
        assert_eq!(ph.get("documents"), Some("/home/u"));
    }

    #[test]
    fn xdg_config_home_wins_over_default() {
        let ph = ph_for(Some("/home/u"), Some("/srv/cfg"));
        assert_eq!(ph.get("config"), Some("/srv/cfg"));
    }

    #[test]
    fn sanitize_drops_blocked_names() {
        let mut raw: HashMap<String, String> = HashMap::new();
        raw.insert("PATH".into(), "/dev/null".into());
        raw.insert("LD_PRELOAD".into(), "x.so".into());
        raw.insert("MY_GAME_OPT".into(), "1".into());
        let out = sanitize_env(&raw);
        assert!(!out.contains_key("PATH"));
        assert!(!out.contains_key("LD_PRELOAD"));
        assert_eq!(out.get("MY_GAME_OPT"), Some(&"1".into()));
        assert_eq!(out.len(), 1);
    }

    #[test]
    fn guard_semantics() {
        let ph = ph_for(None, None);
        assert!(guard_active("1", &ph));
        assert!(guard_active("on", &ph));
        assert!(!guard_active("FALSE", &ph));
        assert!(!guard_active("null", &ph));
        // An absent {input:<id>} substitutes to "" → the guard is inactive.
        assert!(!guard_active("{input:zz_nativera_test_qq}", &ph));
    }
}
