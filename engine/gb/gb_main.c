#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "gb_runtime.h"
#include "gb_ppu.h"

#define GB_WIDTH 160
#define GB_HEIGHT 144
#define SCALE 3

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom.gb>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Error opening ROM file: %s\n", argv[1]);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *rom = malloc(size);
    if (!rom) {
        fclose(f);
        fprintf(stderr, "Memory allocation failed for ROM\n");
        return 1;
    }

    fread(rom, 1, size, f);
    fclose(f);

    printf("Loaded ROM: %s (%ld bytes)\n", argv[1], size);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL Init Failed: %s\n", SDL_GetError());
        free(rom);
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("NativeRa Game Boy Emulator",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          GB_WIDTH * SCALE, GB_HEIGHT * SCALE,
                                          SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "Window Creation Failed: %s\n", SDL_GetError());
        SDL_Quit();
        free(rom);
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        fprintf(stderr, "Renderer Creation Failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        free(rom);
        return 1;
    }

    SDL_Texture *texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             GB_WIDTH, GB_HEIGHT);
    if (!texture) {
        fprintf(stderr, "Texture Creation Failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        free(rom);
        return 1;
    }

    gb_runtime_t runtime;
    gb_ppu_t ppu;

    gb_runtime_init(&runtime, rom, size, NULL, 0);
    gb_ppu_init(&ppu);

    printf("Starting NativeRa Game Boy Emulation Loop (Graphical Mode)...\n");

    int running = 1;
    SDL_Event e;

    /* Game Boy runs at approx 4194304 Hz / 70224 cycles per frame ≈ 59.73 FPS */
    const uint32_t cycles_per_frame = 70224;

    while (running) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                running = 0;
            } else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    running = 0;
                }
            }
        }

        /* Run emulation for one frame */
        uint32_t frame_cycles = 0;
        while (frame_cycles < cycles_per_frame) {
            int cycles = gb_runtime_step(&runtime);
            frame_cycles += cycles;
            gb_ppu_step(&ppu, cycles, runtime.mmu.io);
        }

        /* Update texture with PPU framebuffer */
        SDL_UpdateTexture(texture, NULL, ppu.framebuffer, GB_WIDTH * sizeof(uint32_t));

        /* Render */
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    printf("Emulation stopped. Exiting cleanly.\n");

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    gb_runtime_free(&runtime);
    free(rom);
    return 0;
}
