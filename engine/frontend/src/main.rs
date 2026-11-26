//! NativeRa front-end binary: C-faithful SDL event loop.
//!
//! Scans a ROM directory (argv[1], else `./roms`), renders the menu, and
//! launches the selected entry shell-free via `./run.sh` (Ruling R4).

use std::env;
use std::path::{Path, PathBuf};
use std::time::Duration;

use nativera_frontend::launch::launch_game;
use nativera_frontend::roms;
use nativera_frontend::ui::{self, constants, Action};

fn main() {
    let roms_dir = env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("./roms"));
    let games = roms::with_placeholder(roms::scan(&roms_dir));
    let mut selected = 0;

    let sdl = sdl2::init().expect("SDL_Init");
    let video = sdl.video().expect("SDL video subsystem");
    let window = video
        .window("NativeRa Frontend", constants::WIN_WIDTH, constants::WIN_HEIGHT)
        .position_centered()
        .build()
        .expect("window");
    let mut canvas = window
        .into_canvas()
        .accelerated()
        .present_vsync()
        .build()
        .expect("renderer/canvas");

    // TTF: 24pt DejaVuSans-Bold from the two C system paths; text rendering
    // is skipped entirely when neither exists (`font = None`).
    let ttf = sdl2::ttf::init().expect("TTF_Init");
    let font = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    ]
    .iter()
    .find_map(|p| ttf.load_font(p, 24).ok());

    let mut pump = sdl.event_pump().expect("event pump");

    // The event pump must be drained inside an outer loop. `for event in
    // pump.poll_iter()` alone iterates only the events queued at that instant
    // and then falls through - main returns, the window closes, and the
    // front-end exits after a single frame. That is the whole reason the binary
    // "ran" and vanished instead of showing a menu.
    'running: loop {
        for event in pump.poll_iter() {
            match event {
                sdl2::event::Event::Quit { .. } => break 'running,
                sdl2::event::Event::KeyDown { keycode, .. } => {
                    if let Some(action) = ui::handle_key(keycode, &mut selected, games.len()) {
                        match action {
                            Action::MoveUp | Action::MoveDown => {}
                            Action::Launch => {
                                let entry = &games[selected];
                                if !entry.path.is_empty() {
                                    match launch_game(Path::new("./run.sh"), &entry.path) {
                                        Ok(code) => eprintln!("[fe] exit {code}"),
                                        Err(e) => eprint!("{}", ui::report_launch(&entry.path, &e)),
                                    }
                                }
                            }
                            Action::Quit => break 'running,
                        }
                    }
                }
                _ => {}
            }
        }

        ui::run_frame(
            &mut canvas,
            &games,
            selected,
            "NativeRa - Game Library",
            font.as_ref(),
        )
        .expect("frame");
        std::thread::sleep(Duration::from_millis(constants::FRAME_DELAY_MS as u64));
    }
}
