// Headless smoke test for the Rust front-end's SDL2 + SDL2_ttf stack.
//
// `SDL_VIDEODRIVER=dummy` lets the window/canvas path run without a display.
// A `Once` guards the env mutation so parallel tests don't race it.
static ONCE: std::sync::Once = std::sync::Once::new();

#[test]
fn sdl_ttf_init_headless() {
    ONCE.call_once(|| std::env::set_var("SDL_VIDEODRIVER", "dummy"));
    let sdl = sdl2::init().unwrap();
    let video = sdl.video().unwrap();
    let _ttf = sdl2::ttf::init().unwrap();
    let window = video
        .window("NativeRa Frontend", 1280, 720)
        .position_centered()
        .build()
        .unwrap();
    let _canvas = window.into_canvas().accelerated().present_vsync().build().unwrap();
}
