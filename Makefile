CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O2
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs)
TTF_LIBS = -lSDL2_ttf
CARGO ?= cargo

all: emulator launcher tests

emulator: engine/gb/gb_emulator

engine/gb/gb_emulator: engine/gb/gb_cpu.c engine/gb/gb_mmu.c engine/gb/gb_runtime.c engine/gb/gb_ppu.c engine/gb/gb_main.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $^ $(SDL_LIBS) -o $@

launcher: engine/launcher/nativera-launcher engine/launcher/launch_test

engine/launcher/nativera-launcher: engine/launcher/launcher.c engine/launcher/launch.c engine/launcher/launch.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $^ $(SDL_LIBS) $(TTF_LIBS) -o $@

engine/launcher/launch_test: engine/launcher/launch_test.c engine/launcher/launch.c engine/launcher/launch.h
	$(CC) $(CFLAGS) $^ -o $@

tests:
	$(CC) $(CFLAGS) engine/gb/gb_cpu_test.c engine/gb/gb_cpu.c -o engine/gb/gb_cpu_test
	$(CC) $(CFLAGS) engine/gb/gb_mmu_test.c engine/gb/gb_mmu.c -o engine/gb/gb_mmu_test
	$(CC) $(CFLAGS) engine/gb/gb_runtime_test.c engine/gb/gb_runtime.c engine/gb/gb_cpu.c engine/gb/gb_mmu.c -o engine/gb/gb_runtime_test
	$(CC) $(CFLAGS) engine/gb/gb_ppu_test.c engine/gb/gb_ppu.c -o engine/gb/gb_ppu_test
	$(CC) $(CFLAGS) engine/launcher/launch_test.c engine/launcher/launch.c -o engine/launcher/launch_test
	@echo "Running all tests..."
	./engine/gb/gb_cpu_test
	./engine/gb/gb_mmu_test
	./engine/gb/gb_runtime_test
	./engine/gb/gb_ppu_test
	./engine/launcher/launch_test

# Rust SDL front-end: release binary + full test suite.
# cargo 1.99 takes --manifest-path after the subcommand, not at the top level.
fe:
	$(CARGO) build --manifest-path engine/Cargo.toml --release --bin nativera-fe

fe-tests:
	$(CARGO) test --manifest-path engine/Cargo.toml --release -p nativera_frontend

clean:
	rm -f engine/gb/gb_emulator engine/launcher/nativera-launcher
	rm -f engine/gb/gb_cpu_test engine/gb/gb_mmu_test engine/gb/gb_runtime_test engine/gb/gb_ppu_test
	rm -f engine/launcher/launch_test

.PHONY: all emulator launcher tests fe fe-tests clean
