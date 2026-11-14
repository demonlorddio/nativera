//! Headless smoke for `ui::run_frame`: a real 1280x720 canvas under the SDL
//! dummy driver, with and without a TTF font.

use std::sync::{Mutex, MutexGuard, Once};

use nativera_frontend::roms::GameEntry;
use nativera_frontend::ui;

static ONCE: Once = Once::new();
static SDL_LOCK: Mutex<()> = Mutex::new(());

/// Pin the dummy driver once, then serialize the two SDL-using tests: one
/// process, one global SDL context.
fn sdl_guard() -> MutexGuard<'static, ()> {
    ONCE.call_once(|| std::env::set_var("SDL_VIDEODRIVER", "dummy"));
    SDL_LOCK.lock().unwrap_or_else(|p| p.into_inner())
}

fn sample_games() -> Vec<GameEntry> {
    vec![
        GameEntry {
            name: "Alpha".to_string(),
            path: "roms/alpha.gb".into(),
        },
        GameEntry {
            name: "Beta".to_string(),
            path: "roms/beta.gb".into(),
        },
        GameEntry {
            name: "Gamma".to_string(),
            path: "roms/gamma.sh".into(),
        },
    ]
}

#[test]
fn frame_renders_headless_with_font() {
    let _guard = sdl_guard();
    let sdl = sdl2::init().expect("sdl2::init under dummy driver");
    let video = sdl.video().expect("video subsystem");
    let window = video
        .window("NativeRa Frontend", 1280, 720)
        .position_centered()
        .build()
        .expect("window");
    let mut canvas = window
        .into_canvas()
        .accelerated()
        .present_vsync()
        .build()
        .expect("canvas");

    let ttf = sdl2::ttf::init().expect("ttf::init is video-independent");
    let font = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    ]
    .iter()
    .find_map(|p| ttf.load_font(p, 24).ok());
    let games = sample_games();
    ui::run_frame(&mut canvas, &games, 0, "NativeRa - Game Library", font.as_ref())
        .expect("frame with font");
}

#[test]
fn frame_renders_headless_without_font() {
    let _guard = sdl_guard();
    let sdl = sdl2::init().expect("sdl2::init under dummy driver");
    let video = sdl.video().expect("video subsystem");
    let window = video
        .window("NativeRa Frontend", 1280, 720)
        .position_centered()
        .build()
        .expect("window");
    let mut canvas = window
        .into_canvas()
        .accelerated()
        .present_vsync()
        .build()
        .expect("canvas");

    let games = sample_games();
    ui::run_frame(
        &mut canvas,
        &games,
        1,
        "NativeRa - Game Library",
        None::<&sdl2::ttf::Font>,
    )
    .expect("textless frame");
}
