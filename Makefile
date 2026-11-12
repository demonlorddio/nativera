CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs)
TTF_LIBS = -lSDL2_ttf

all: emulator launcher tests

emulator: engine/gb/gb_emulator

engine/gb/gb_emulator: engine/gb/gb_cpu.c engine/gb/gb_mmu.c engine/gb/gb_runtime.c engine/gb/gb_ppu.c engine/gb/gb_main.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $^ $(SDL_LIBS) -o $@

launcher: engine/launcher/nativera-launcher

engine/launcher/nativera-launcher: engine/launcher/launcher.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $< $(SDL_LIBS) $(TTF_LIBS) -o $@

tests:
	$(CC) $(CFLAGS) engine/gb/gb_cpu_test.c engine/gb/gb_cpu.c -o engine/gb/gb_cpu_test
	$(CC) $(CFLAGS) engine/gb/gb_mmu_test.c engine/gb/gb_mmu.c -o engine/gb/gb_mmu_test
	$(CC) $(CFLAGS) engine/gb/gb_runtime_test.c engine/gb/gb_runtime.c engine/gb/gb_cpu.c engine/gb/gb_mmu.c -o engine/gb/gb_runtime_test
	$(CC) $(CFLAGS) engine/gb/gb_ppu_test.c engine/gb/gb_ppu.c -o engine/gb/gb_ppu_test
	@echo "Running all tests..."
	./engine/gb/gb_cpu_test
	./engine/gb/gb_mmu_test
	./engine/gb/gb_runtime_test
	./engine/gb/gb_ppu_test

clean:
	rm -f engine/gb/gb_emulator engine/launcher/nativera-launcher
	rm -f engine/gb/gb_cpu_test engine/gb/gb_mmu_test engine/gb/gb_runtime_test engine/gb/gb_ppu_test

.PHONY: all emulator launcher tests clean
