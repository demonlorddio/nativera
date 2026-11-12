#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define MAX_GAMES 100

typedef struct {
    char name[256];
    char path[512];
} GameEntry;

GameEntry games[MAX_GAMES];
int game_count = 0;
int selected_index = 0;

void scan_roms(const char *base_dir) {
    DIR *d = opendir(base_dir);
    if (!d) return;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type == DT_REG || dir->d_type == DT_LNK) {
            const char *ext = strrchr(dir->d_name, '.');
            if (ext && (strcmp(ext, ".gb") == 0 || strcmp(ext, ".sh") == 0)) {
                if (game_count < MAX_GAMES) {
                    snprintf(games[game_count].name, sizeof(games[game_count].name), "%s", dir->d_name);
                    snprintf(games[game_count].path, sizeof(games[game_count].path), "%s/%s", base_dir, dir->d_name);
                    game_count++;
                }
            }
        }
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL Init Failed: %s\n", SDL_GetError());
        return 1;
    }

    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF Init Failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("NativeRa Frontend",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "Window Creation Failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        fprintf(stderr, "Renderer Creation Failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    // Attempt to load a default font, or fall back to basic rendering
    TTF_Font *font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 24);
    if (!font) {
        // Try another common path
        font = TTF_OpenFont("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", 24);
    }

    // Scan ROMs from ./roms or argument
    const char *rom_dir = (argc > 1) ? argv[1] : "./roms";
    scan_roms(rom_dir);

    // If no games found, add a placeholder
    if (game_count == 0) {
        strcpy(games[0].name, "No ROMs found (Place .gb or .sh in roms/)");
        strcpy(games[0].path, "");
        game_count = 1;
    }

    int running = 1;
    SDL_Event e;

    while (running) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_UP:
                    case SDLK_w:
                        if (selected_index > 0) selected_index--;
                        break;
                    case SDLK_DOWN:
                    case SDLK_s:
                        if (selected_index < game_count - 1) selected_index++;
                        break;
                    case SDLK_RETURN:
                    case SDLK_SPACE:
                        if (game_count > 0 && strlen(games[selected_index].path) > 0) {
                            char cmd[1024];
                            snprintf(cmd, sizeof(cmd), "./run.sh \"%s\"", games[selected_index].path);
                            system(cmd);
                        }
                        break;
                    case SDLK_ESCAPE:
                        running = 0;
                        break;
                }
            }
        }

        // Render Background (EmulationStation dark blue/gray theme)
        SDL_SetRenderDrawColor(renderer, 24, 26, 33, 255);
        SDL_RenderClear(renderer);

        // Render Header
        SDL_SetRenderDrawColor(renderer, 40, 44, 52, 255);
        SDL_Rect header_rect = {0, 0, SCREEN_WIDTH, 80};
        SDL_RenderFillRect(renderer, &header_rect);

        if (font) {
            SDL_Color white = {255, 255, 255, 255};
            SDL_Surface *surf = TTF_RenderText_Blended(font, "NativeRa - Game Library", white);
            if (surf) {
                SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
                SDL_Rect dest = {40, 25, surf->w, surf->h};
                SDL_RenderCopy(renderer, tex, NULL, &dest);
                SDL_FreeSurface(surf);
                SDL_DestroyTexture(tex);
            }
        }

        // Render Game List
        for (int i = 0; i < game_count; i++) {
            int y_pos = 120 + (i * 50);
            if (i == selected_index) {
                // Selection Highlight
                SDL_SetRenderDrawColor(renderer, 65, 105, 225, 255);
                SDL_Rect hl_rect = {40, y_pos - 5, SCREEN_WIDTH - 80, 40};
                SDL_RenderFillRect(renderer, &hl_rect);
            }

            if (font) {
                SDL_Color text_color = (i == selected_index) ? (SDL_Color){255, 255, 255, 255} : (SDL_Color){180, 180, 180, 255};
                SDL_Surface *surf = TTF_RenderText_Blended(font, games[i].name, text_color);
                if (surf) {
                    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
                    SDL_Rect dest = {60, y_pos, surf->w, surf->h};
                    SDL_RenderCopy(renderer, tex, NULL, &dest);
                    SDL_FreeSurface(surf);
                    SDL_DestroyTexture(tex);
                }
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
