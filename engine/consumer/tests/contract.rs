//! Contract test: the Rust consumer must pass the same 10 self-test checks
//! the Python runner passed against the real Asura's Wrath manifest
//! (verify present/missing, exit-code mapping, config new/ensure
//! idempotence, saves collection, {config} placeholder resolution).
//!
//! Hermetic: the manifest is embedded verbatim below, and a fake BYO disc +
//! install tree are built in a temp dir. No network, no real package.

use std::path::Path;
use std::process::{Command, Output};

// Verbatim copy of /home/dio/Games/AsurasWrathRecomp-linux-x86_64/recomplauncher.json
static MANIFEST: &str = r#"{
  "$schema": "https://recomplauncher.com/schema/recomplauncher.json",
  "format": 1,
  "name": "Asura's Wrath Recompiled",
  "repo": "demonlorddio/Asuras_wrath_recomp",
  "game": {
    "kind": "iso_or_folder",
    "label": "Your own copy of Asura's Wrath for PlayStation 3 (BLUS30721, USA): the .iso, or the game folder that has PS3_GAME in it.",
    "check": { "file": "PS3_GAME/PARAM.SFO" }
  },
  "start": {
    "program": ["asuraswrath"],
    "args": ["--game", "{game}"]
  },
  "prepare": {
    "program": ["asuraswrath"],
    "args": ["--prepare", "--game", "{game}"],
    "label": "Compiling the game into a native module",
    "timeout": 1800
  },
  "config": [
    {
      "id": "main",
      "format": "json",
      "file": [{ "path": "{install}/settings.json" }],
      "new": {
        "fullscreen": "true",
        "vsync": "false",
        "perf_overlay": "false",
        "resolution_scale": "150",
        "vblank_rate": "60",
        "anisotropic_filter": "16",
        "upscaling": "\"FSR\"",
        "cheat_60fps": "true"
      }
    }
  ],
  "settings": [
    {
      "id": "resolution",
      "label": "Resolution",
      "choices": [
        { "label": "720p", "set": { "resolution_scale": "100" } },
        { "label": "1080p", "set": { "resolution_scale": "150" } }
      ],
      "defaults": { "resolution_scale": "150" }
    }
  ],
  "saves": [
    "{config}/rpcs3/dev_hdd0/home/00000001/savedata",
    "{install}/settings.json",
    "{install}/recomp.cfg"
  ],
  "folders": {
    "saves": [{ "path": "{config}/rpcs3/dev_hdd0/home/00000001/savedata" }],
    "logs": [{ "path": "{home}/.cache/rpcs3" }]
  },
  "exit_codes": {
    "2": "It couldn't find your copy of Asura's Wrath. Pick it again on the game's page.",
    "3": "The one-time compile into a native module failed. The engine's log is in the Logs folder."
  },
  "looks": { "graphics": "vulkan" },
  "actions": [
    { "id": "saves", "label": "Saves", "help": "Your Asura's Wrath save data.", "open": "saves" },
    { "id": "logs", "label": "Logs", "help": "The engine's log, useful when something breaks.", "open": "logs" }
  ]
}"#;

fn run(bin: &str, install: &Path, phase: &str, extra: &[String], cfghome: &Path) -> Output {
    let mut c = Command::new(bin);
    c.arg(install).arg(phase);
    for e in extra {
        c.arg(e);
    }
    c.env("XDG_CONFIG_HOME", cfghome)
        .current_dir(std::env::temp_dir());
    c.output().expect("spawn the consumer binary")
}

#[test]
fn recomp_launcher_contract_holds() {
    let root = std::env::temp_dir().join(format!("recomp-contract-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    let install = root.join("install");
    let cfghome = root.join("xdg");
    let disc = root.join("disc");
    std::fs::create_dir_all(&install).unwrap();
    std::fs::create_dir_all(&cfghome).unwrap();
    std::fs::create_dir_all(disc.join("PS3_GAME")).unwrap();
    std::fs::write(disc.join("PS3_GAME/PARAM.SFO"), "fake sfo\n").unwrap();
    std::fs::write(install.join("recomplauncher.json"), MANIFEST).unwrap();
    std::fs::write(install.join("recomp.cfg"), "pin\n").unwrap();

    let bin = env!("CARGO_BIN_EXE_recomp-launcher");
    let disc_s = disc.to_str().unwrap().to_string();
    let install_s = install.to_str().unwrap().to_string();

    // 1) verify present -> rc 0; 2) logs "OK"
    let r = run(bin, &install, "verify", &["--game".into(), disc_s.clone()], &cfghome);
    let out = String::from_utf8_lossy(&r.stdout);
    assert!(r.status.success(), "verify present -> rc 0, got {:?}\n{out}", r.status.code());
    assert!(out.contains("OK"), "verify logs OK: {out}");

    // 3) verify missing -> rc 2; 4) surfaces exit_codes["2"]
    let missing = root.join("nope").to_str().unwrap().to_string();
    let r = run(bin, &install, "verify", &["--game".into(), missing], &cfghome);
    let out = String::from_utf8_lossy(&r.stdout);
    assert_eq!(r.status.code(), Some(2), "verify missing -> rc 2\n{out}");
    assert!(
        out.contains("It couldn't find your copy of Asura's Wrath"),
        "missing surfaces exit_codes[2]: {out}"
    );

    // 5) config first run -> rc 0; 6) new defaults written
    let r = run(bin, &install, "config", &["--game".into(), disc_s.clone()], &cfghome);
    let out = String::from_utf8_lossy(&r.stdout);
    assert!(r.status.success(), "config first run -> rc 0\n{out}");
    let settings_path = install.join("settings.json");
    let s: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(&settings_path).unwrap()).unwrap();
    assert_eq!(
        s.get("resolution_scale"),
        Some(&serde_json::json!("150")),
        "new defaults written: {s}"
    );

    // 7) new does not clobber user edits (re-run config)
    let mut s2 = s.clone();
    s2["fullscreen"] = serde_json::json!(false);
    s2["vblank_rate"] = serde_json::json!(120);
    std::fs::write(&settings_path, serde_json::to_string_pretty(&s2).unwrap()).unwrap();
    let r = run(bin, &install, "config", &["--game".into(), disc_s.clone()], &cfghome);
    let out = String::from_utf8_lossy(&r.stdout);
    assert!(r.status.success(), "config re-run -> rc 0\n{out}");
    let s3: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(&settings_path).unwrap()).unwrap();
    assert_eq!(s3["fullscreen"], false, "user edits kept: {s3}");
    assert_eq!(s3["vblank_rate"], 120, "user edits kept: {s3}");
    assert_eq!(
        s3.get("resolution_scale"),
        Some(&serde_json::json!("150")),
        "un-edited keys untouched: {s3}"
    );

    // 8) saves collection: "kept 3 of 3"; 9) 3 names incl. "settings";
    // 10) "savedata" name proves {config} resolved to XDG_CONFIG_HOME
    let savedata = cfghome.join("rpcs3/dev_hdd0/home/00000001/savedata");
    std::fs::create_dir_all(&savedata).unwrap();
    std::fs::write(savedata.join("SAVE1"), "x").unwrap();
    let r = run(bin, &install, "saves", &["--game".into(), disc_s], &cfghome);
    let out = String::from_utf8_lossy(&r.stdout);
    assert!(out.contains("kept 3 of 3"), "saves kept 3 of 3: {out}");
    let mut names: Vec<String> = std::fs::read_dir(install.join(".recomp/saves"))
        .unwrap()
        .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    assert_eq!(names.len(), 3, "3 targets collected: {names:?}");
    assert!(
        names.iter().any(|n| n.contains("settings")),
        "settings collected: {names:?}"
    );
    assert!(
        names.iter().any(|n| n.contains("savedata")),
        "saves found {{config}}/rpcs3 savedata: {names:?}"
    );

    let _ = std::fs::remove_dir_all(&root);
    let _ = install_s;
}
