//! Menu UI: C-faithful frame rendering and keyboard handling.
//!
//! Layout values mirror the C front-end (window 1280x720, header band,
//! 50px rows starting at y=120, title at (40,25), 16ms frame delay).

use sdl2::keyboard::Keycode;

use crate::launch::LaunchError;
use crate::roms::GameEntry;

/// One key event's effect on the menu.
#[derive(Debug, PartialEq, Eq)]
pub enum Action {
    MoveUp,
    MoveDown,
    Launch,
    Quit,
}

/// C front-end layout constants. `sdl2::rect::Rect::new` is not const in
/// sdl2 0.38, so rectangle values are exposed through the `*_rect`
/// functions and scalars stay `pub const`.
pub mod constants {
    use sdl2::pixels::Color;

    pub const WIN_WIDTH: u32 = 1280;
    pub const WIN_HEIGHT: u32 = 720;
    pub const BG: Color = Color::RGB(24, 26, 33);
    pub const HEADER_X: i32 = 0;
    pub const HEADER_Y: i32 = 0;
    pub const HEADER_W: u32 = 1280;
    pub const HEADER_H: u32 = 80;
    pub const HEADER_COLOR: Color = Color::RGB(40, 44, 52);
    pub const HIGHLIGHT_X: i32 = 40;
    pub const HIGHLIGHT_W: u32 = 1200;
    pub const HIGHLIGHT_H: u32 = 40;
    pub const HIGHLIGHT_PAD: i32 = 5;
    pub const HIGHLIGHT_COLOR: Color = Color::RGB(65, 105, 225);
    pub const TEXT_X: i32 = 60;
    pub const TITLE_X: i32 = 40;
    pub const TITLE_Y: i32 = 25;
    pub const SELECTED_COLOR: Color = Color::RGB(255, 255, 255);
    pub const UNSELECTED_COLOR: Color = Color::RGB(180, 180, 180);
    pub const ROW_Y0: i32 = 120;
    pub const ROW_STEP: i32 = 50;
    pub const FRAME_DELAY_MS: u32 = 16;
}

/// y-coordinate of row `i`, i.e. `120 + i*50`.
pub fn row_y(i: usize) -> i32 {
    constants::ROW_Y0 + i as i32 * constants::ROW_STEP
}

/// The 1200x40 highlight band for row `i` (y offset by -5, as in C).
pub fn highlight_rect(i: usize) -> sdl2::rect::Rect {
    sdl2::rect::Rect::new(
        constants::HIGHLIGHT_X,
        row_y(i) - constants::HIGHLIGHT_PAD,
        constants::HIGHLIGHT_W,
        constants::HIGHLIGHT_H,
    )
}

/// The title text destination rect `(40, 25)`.
pub fn title_rect() -> sdl2::rect::Rect {
    sdl2::rect::Rect::new(constants::TITLE_X, constants::TITLE_Y, 0, 0)
}

/// The header band rect `(0, 0, 1280, 80)`.
pub fn header_rect() -> sdl2::rect::Rect {
    sdl2::rect::Rect::new(
        constants::HEADER_X,
        constants::HEADER_Y,
        constants::HEADER_W,
        constants::HEADER_H,
    )
}

/// Map a key-down event to a menu action, moving `selected` in place.
///
/// Up/W move up (clamped at 0), Down/S move down (clamped at `count-1`),
/// Return/Space launch, Escape quits, anything else is ignored.
pub fn handle_key(code: Option<Keycode>, selected: &mut usize, count: usize) -> Option<Action> {
    match code {
        Some(Keycode::Up) | Some(Keycode::W) => {
            *selected = selected.saturating_sub(1);
            Some(Action::MoveUp)
        }
        Some(Keycode::Down) | Some(Keycode::S) => {
            *selected = (*selected + 1).min(count.saturating_sub(1));
            Some(Action::MoveDown)
        }
        Some(Keycode::Return) | Some(Keycode::Space) => Some(Action::Launch),
        Some(Keycode::Escape) => Some(Action::Quit),
        _ => None,
    }
}

/// The exact stderr line `main` prints when a launch fails.
pub fn report_launch(rom: &std::ffi::OsStr, err: &LaunchError) -> String {
    format!("[fe] launch failed ({}): {err}\n", rom.to_string_lossy())
}

/// Render one full C-faithful frame: bg clear, header band, optional title
/// and row text via `font` (skipped when `None`), highlight band on the
/// selected row, present.
///
/// `Canvas` is a concrete struct in sdl2 0.38 (not a trait), so the
/// parameter is the concrete `&mut Canvas<Window>` (Ruling R5).
pub fn run_frame(
    canvas: &mut sdl2::render::Canvas<sdl2::video::Window>,
    games: &[GameEntry],
    selected: usize,
    title: &str,
    font: Option<&sdl2::ttf::Font>,
) -> Result<(), String> {
    canvas.set_draw_color(constants::BG);
    canvas.clear();
    canvas.set_draw_color(constants::HEADER_COLOR);
    canvas.fill_rect(header_rect())?;

    if let Some(font) = font {
        let texture_creator = canvas.texture_creator();

        let surface = font
            .render(title)
            .blended(constants::SELECTED_COLOR)
            .map_err(|e| e.to_string())?;
        let texture = texture_creator
            .create_texture_from_surface(&surface)
            .map_err(|e| e.to_string())?;
        canvas.copy(&texture, None, Some(title_rect()))?;

        for (i, game) in games.iter().enumerate() {
            if i == selected {
                canvas.set_draw_color(constants::HIGHLIGHT_COLOR);
                canvas.fill_rect(highlight_rect(i))?;
            }
            let color = if i == selected {
                constants::SELECTED_COLOR
            } else {
                constants::UNSELECTED_COLOR
            };
            let surface = font
                .render(&game.name)
                .blended(color)
                .map_err(|e| e.to_string())?;
            let texture = texture_creator
                .create_texture_from_surface(&surface)
                .map_err(|e| e.to_string())?;
            canvas.copy(
                &texture,
                None,
                Some(sdl2::rect::Rect::new(constants::TEXT_X, row_y(i), 0, 0)),
            )?;
        }
    }

    canvas.present();
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn highlight_row_rects_match_c() {
        assert_eq!(highlight_rect(0), sdl2::rect::Rect::new(40, 115, 1200, 40));
        assert_eq!(highlight_rect(3), sdl2::rect::Rect::new(40, 265, 1200, 40));
        assert_eq!(row_y(2), 220);
        assert_eq!(title_rect(), sdl2::rect::Rect::new(40, 25, 0, 0));
    }

    #[test]
    fn key_mapping() {
        let mut sel = 2usize;
        assert_eq!(handle_key(Some(Keycode::Up), &mut sel, 5), Some(Action::MoveUp));
        assert_eq!(sel, 1);

        sel = 0;
        assert_eq!(
            handle_key(Some(Keycode::W), &mut sel, 5),
            Some(Action::MoveUp)
        );
        assert_eq!(sel, 0);

        sel = 4;
        assert_eq!(handle_key(Some(Keycode::S), &mut sel, 5), Some(Action::MoveDown));
        assert_eq!(sel, 4);

        sel = 2;
        assert_eq!(
            handle_key(Some(Keycode::Space), &mut sel, 5),
            Some(Action::Launch)
        );
        assert_eq!(sel, 2);

        sel = 2;
        assert_eq!(
            handle_key(Some(Keycode::Escape), &mut sel, 5),
            Some(Action::Quit)
        );

        sel = 2;
        assert_eq!(handle_key(Some(Keycode::A), &mut sel, 5), None);
        assert_eq!(sel, 2);
    }

    #[test]
    fn launch_error_prints_and_continues() {
        let err = LaunchError::Spawn(std::io::Error::new(
            std::io::ErrorKind::NotFound,
            "run.sh missing",
        ));
        let line = report_launch(std::ffi::OsStr::new("roms/a.gb"), &err);
        assert_eq!(line, format!("[fe] launch failed (roms/a.gb): {err}\n"));
    }
}
