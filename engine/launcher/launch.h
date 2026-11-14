#ifndef NATIVERA_LAUNCH_H
#define NATIVERA_LAUNCH_H

#include <sys/types.h>

/* Run `program` with `argv` (NULL-terminated) and no shell in between.
 * The path in argv is never parsed, so a ROM name containing quotes,
 * spaces or shell metacharacters cannot escape.
 *
 * Waits for the child. Returns the child's exit status (0-255), 128+signal
 * when it dies by signal, or -1 when the child could not be started (the
 * reason is printed to stderr).
 */
int nativera_spawn_wait(const char *program, char *const argv[]);

/* The launch the front-end performs for one library entry: run `run_sh`
 * with the ROM path as its single argument. `out_status` receives the
 * child's status (see nativera_spawn_wait). Returns 0 on a clean exit, -1
 * when the child could not be started. */
int nativera_launch_game(const char *run_sh, const char *rom_path, int *out_status);

#endif
