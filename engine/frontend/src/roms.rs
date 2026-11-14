//! ROM directory scanning: list `.gb`/`.sh` ROMs, capped at 100, with a
//! placeholder entry when the directory is empty or missing.

use std::ffi::OsString;
use std::path::Path;

/// One menu row: a lossy display name plus a byte-exact launch path.
pub struct GameEntry {
    pub name: String,
    pub path: OsString,
}

/// Menu text shown when a ROM directory has no `.gb`/`.sh` entries.
pub const PLACEHOLDER_NAME: &str = "No ROMs found (Place .gb or .sh in roms/)";

/// Cap on menu rows, matching the C front-end's `MAX_GAMES`.
const MAX_GAMES: usize = 100;

/// Scan `dir` for regular files (and symlinks to files) with a case-sensitive
/// `.gb` or `.sh` extension, capped at [`MAX_GAMES`].
///
/// A missing or unreadable directory yields an empty list; the UI layer turns
/// that into the placeholder row via [`with_placeholder`].
pub fn scan(dir: &Path) -> Vec<GameEntry> {
    let entries = match dir.read_dir() {
        Ok(entries) => entries,
        Err(_) => return Vec::new(),
    };
    let mut games = Vec::new();
    for entry in entries.flatten() {
        if games.len() >= MAX_GAMES {
            break;
        }
        // The C front-end accepted DT_REG and DT_LNK entries. On Linux,
        // `file_type()` reports the entry's own type (d_type) and does not
        // follow the link, so a live symlink needs the checked-follow branch;
        // a broken or dir-pointing link is dropped (a dangling ROM link is
        // useless).
        let ft = match entry.file_type() {
            Ok(ft) => ft,
            Err(_) => continue,
        };
        if !ft.is_file() && !(ft.is_symlink() && entry.path().is_file()) {
            continue;
        }
        // Case-sensitive: `UPPER.GB` is not a ROM. std's `extension()` does
        // not treat a leading dot as a separator (Ruling R1).
        let ext_ok = entry.path().extension().map_or(false, |ext| ext == "gb" || ext == "sh");
        if !ext_ok {
            continue;
        }
        games.push(GameEntry {
            name: entry.file_name().to_string_lossy().into_owned(),
            path: entry.path().into_os_string(),
        });
    }
    games
}

/// Return `games` if non-empty, else the single placeholder row with an empty
/// path so the UI can render it but never launch it.
pub fn with_placeholder(mut games: Vec<GameEntry>) -> Vec<GameEntry> {
    if games.is_empty() {
        games.push(GameEntry {
            name: PLACEHOLDER_NAME.to_string(),
            path: OsString::new(),
        });
    }
    games
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn test_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "nativera-fe-roms-{tag}-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn scans_gb_and_sh() {
        let dir = test_dir("scan");
        std::fs::write(dir.join("a.gb"), "x").unwrap();
        std::fs::write(dir.join("b.sh"), "x").unwrap();
        std::fs::write(dir.join("c.txt"), "x").unwrap();
        std::fs::write(dir.join("UPPER.GB"), "x").unwrap();
        std::fs::create_dir(dir.join("tricky.gb")).unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink(dir.join("a.gb"), dir.join("link.gb")).unwrap();

        let mut games = scan(&dir);
        games.sort_by(|a, b| a.name.cmp(&b.name));
        #[cfg(unix)]
        let expected = vec!["a.gb", "b.sh", "link.gb"];
        #[cfg(not(unix))]
        let expected = vec!["a.gb", "b.sh"];
        let names: Vec<&str> = games.iter().map(|g| g.name.as_str()).collect();
        assert_eq!(names, expected);
    }

    #[test]
    fn path_is_dir_joined() {
        let dir = test_dir("path");
        std::fs::write(dir.join("a.gb"), "x").unwrap();
        let games = scan(&dir);
        let entry = games.iter().find(|g| g.name == "a.gb").unwrap();
        let expected: OsString = dir.join("a.gb").as_os_str().to_os_string();
        assert_eq!(entry.path, expected);
    }

    #[test]
    fn empty_dir_gets_placeholder() {
        let dir = test_dir("empty");
        let games = scan(&dir);
        assert!(games.is_empty());
        let with_ph = with_placeholder(games);
        assert_eq!(with_ph.len(), 1);
        assert_eq!(with_ph[0].name, PLACEHOLDER_NAME);
        assert!(with_ph[0].path.is_empty());

        // Non-empty input passes through unchanged.
        std::fs::write(dir.join("a.gb"), "x").unwrap();
        let games = scan(&dir);
        let passed = with_placeholder(games);
        assert_eq!(passed.len(), 1);
        assert_eq!(passed[0].name, "a.gb");
    }

    #[test]
    fn missing_dir_is_empty() {
        let dir = test_dir("missing");
        let games = scan(&dir.join("does-not-exist"));
        assert!(games.is_empty());
    }

    #[test]
    fn cap_at_100() {
        let dir = test_dir("cap");
        for i in 0..105 {
            std::fs::write(dir.join(format!("g{i:03}.gb")), "x").unwrap();
        }
        let games = scan(&dir);
        assert_eq!(games.len(), 100);
    }

    #[test]
    #[cfg(unix)]
    fn dead_or_dir_symlinks_are_dropped() {
        use std::os::unix::fs::symlink;
        let dir = test_dir("links");
        std::fs::write(dir.join("real.gb"), "x").unwrap();
        symlink(dir.join("nope.gb"), dir.join("dead.gb")).unwrap(); // broken link
        std::fs::create_dir(dir.join("sub")).unwrap();
        symlink(dir.join("sub"), dir.join("sublink.gb")).unwrap(); // dir link
        let games = scan(&dir);
        assert_eq!(
            games.iter().map(|g| g.name.as_str()).collect::<Vec<_>>(),
            vec!["real.gb"]
        );
    }

    #[test]
    #[cfg(unix)]
    fn non_utf8_name_survives() {
        use std::os::unix::ffi::OsStringExt;
        let dir = test_dir("nonutf8");
        let raw = OsString::from_vec(b"caf\x07f.gb".to_vec()); // \x07 is non-UTF-8
        std::fs::write(dir.join(&raw), "x").unwrap();
        let games = scan(&dir);
        let entry = games.iter().find(|g| g.name.ends_with(".gb")).unwrap();
        let expected = dir.join(&raw).as_os_str().to_os_string();
        assert_eq!(entry.path, expected);
        assert!(entry.name.ends_with(".gb"));
    }
}
