//! On-target CLI: `recomp-launcher [title] [phase] [--game DIR]
//! [--force-prepare] [--env K=V]…`
//!
//! Hand-rolled on purpose: the distro ships no external CLI framework into
//! the boot image. Phases mirror the Python `main` dispatch — `run` is the
//! default, and `start` is an alias that runs the full sequence too.

use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut title: Option<String> = None;
    let mut phase = "run".to_string();
    let mut positional: Vec<String> = Vec::new();
    let mut game: Option<String> = None;
    let mut force_prepare = false;
    let mut overrides: Vec<(String, String)> = Vec::new();

    let mut i = 0;
    while i < args.len() {
        let a = args[i].clone();
        if a == "--game" {
            i += 1;
            if i >= args.len() {
                eprintln!("--game needs a value");
                return ExitCode::from(2);
            }
            game = Some(args[i].clone());
        } else if a == "--force-prepare" {
            force_prepare = true;
        } else if a == "--env" {
            i += 1;
            if i >= args.len() {
                eprintln!("--env needs K=V");
                return ExitCode::from(2);
            }
            let (k, v) = parse_env_kv(&args[i]);
            overrides.push((k, v));
        } else if a.starts_with('-') && a.len() > 1 {
            eprintln!("unknown option {a}");
            return ExitCode::from(2);
        } else {
            positional.push(a);
        }
        i += 1;
    }
    if let Some(p) = positional.get(0) {
        title = Some(p.clone());
    }
    if let Some(p) = positional.get(1) {
        phase = p.clone();
    }
    if let Some(p) = positional.get(2) {
        eprintln!("unexpected argument {p}");
        return ExitCode::from(2);
    }

    // Python: `args.title or os.environ.get("NATIVERA_TITLE")`, else error.
    let title = match title.or_else(|| std::env::var("NATIVERA_TITLE").ok()) {
        Some(t) => t,
        None => {
            eprintln!("need a title dir (positional or NATIVERA_TITLE)");
            return ExitCode::from(2);
        }
    };
    if !matches!(
        phase.as_str(),
        "run" | "verify" | "prepare" | "config" | "start" | "saves"
    ) {
        eprintln!("invalid phase {phase} (run, verify, prepare, config, start, saves)");
        return ExitCode::from(2);
    }

    let mut l = match recomp_launcher::Launcher::new(&title, game.as_deref(), &overrides) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::FAILURE;
        }
    };

    let res: Result<(), recomp_launcher::Exit> = match phase.as_str() {
        "verify" => l.verify(),
        "prepare" => l.verify().and_then(|()| l.prepare(force_prepare)),
        "config" => l.verify().and_then(|()| l.config()),
        "saves" => {
            l.saves_after();
            Ok(())
        }
        // "run" and "start" both run the full sequence — Python's "start"
        // falls into the run() branch of its dispatch too.
        _ => l.run(force_prepare),
    };

    match res {
        Ok(()) => ExitCode::SUCCESS,
        Err(recomp_launcher::Exit(code)) => ExitCode::from((code & 0xff) as u8),
    }
}

/// Python `kv.partition("=")`: split at the first `=`; no `=` → `(s, "")`.
fn parse_env_kv(s: &str) -> (String, String) {
    match s.find('=') {
        Some(idx) => (s[..idx].to_string(), s[idx + 1..].to_string()),
        None => (s.to_string(), String::new()),
    }
}
