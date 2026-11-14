//! Shell-free game launch: `run.sh <rom>` with the ROM as one raw argument.
//!
//! The C contract: exit 0-255 → that code; killed by signal → 128 + signum;
//! spawn/wait I/O failure → error. `rom` never passes through a shell, so a
//! ROM name containing `;`, spaces, or quotes is inert.

use std::ffi::OsStr;
use std::path::Path;
use std::process::Command;

#[cfg(unix)]
use std::os::unix::process::ExitStatusExt;

/// I/O failure while launching a game. Distinguishes the spawn and wait
/// steps; the C contract lumped both into `-1` (Ruling R3). No `PartialEq`:
/// the inner `std::io::Error` is not comparable.
#[derive(Debug)]
pub enum LaunchError {
    Spawn(std::io::Error),
    Wait(std::io::Error),
}

impl std::fmt::Display for LaunchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Spawn(e) => write!(f, "spawn failed: {e}"),
            Self::Wait(e) => write!(f, "wait failed: {e}"),
        }
    }
}

impl std::error::Error for LaunchError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Spawn(e) => Some(e),
            Self::Wait(e) => Some(e),
        }
    }
}

/// Spawn `run_sh` with `rom` as its single raw argument, wait for it, and
/// return its exit status: a normal exit as `i32` (0-255) or `128 + signum`
/// when killed by a signal.
pub fn launch_game(run_sh: &Path, rom: &OsStr) -> Result<i32, LaunchError> {
    let mut child = Command::new(run_sh).arg(rom).spawn().map_err(LaunchError::Spawn)?;
    let status = child.wait().map_err(LaunchError::Wait)?;
    #[cfg(unix)]
    {
        // A non-exited status on this platform always carries a signal; the
        // `unwrap_or(0)` keeps the 128-fallback total.
        return Ok(status.code().map(|c| c as i32).unwrap_or(128 + status.signal().unwrap_or(0)));
    }
    #[cfg(not(unix))]
    {
        let _ = status;
        unimplemented!("nativera targets Linux")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn test_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "nativera-fe-launch-{tag}-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn script(dir: &Path, name: &str, body: &str) -> PathBuf {
        let p = dir.join(name);
        std::fs::write(&p, body).unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o755)).unwrap();
        }
        p
    }

    /// The C launch test's injection payload, passed as one raw argv entry.
    const ROM_NAME: &str = "game; rm -rf / \"quoted'.gb";

    #[test]
    fn injection_payload_arrives_byte_exact() {
        let dir = test_dir("inject");
        let capture = dir.join("capture");
        let run_sh = script(
            &dir,
            "run.sh",
            &format!(
                "#!/bin/sh\nprintf '%s' \"$1\" > \"{}\"\nexit 7\n",
                capture.display()
            ),
        );
        let rom = dir.join(ROM_NAME);
        match launch_game(&run_sh, rom.as_os_str()) {
            Ok(code) => assert_eq!(code, 7),
            Err(e) => panic!("expected Ok(7), got Err({e})"),
        }
        let captured = std::fs::read(&capture).unwrap();
        assert_eq!(captured, rom.as_os_str().to_string_lossy().as_bytes());
    }

    #[test]
    fn missing_program_is_spawn_error() {
        let dir = test_dir("missing");
        let rom = dir.join("r.gb");
        let res = launch_game(Path::new("/nonexistent/nativera-fe-run.sh"), rom.as_os_str());
        assert!(matches!(res, Err(LaunchError::Spawn(_))));
    }

    #[test]
    fn signal_kill_maps_to_128_plus_signum() {
        let dir = test_dir("sig");
        let run_sh = script(&dir, "run.sh", "#!/bin/sh\nkill -TERM $$\n");
        let rom = dir.join("r.gb");
        match launch_game(&run_sh, rom.as_os_str()) {
            Ok(code) => assert_eq!(code, 143),
            Err(e) => panic!("expected Ok(143), got Err({e})"),
        }
    }
}
