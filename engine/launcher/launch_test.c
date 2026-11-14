/* Lane B: the launcher must hand the ROM path to run.sh as a raw argument,
 * never through a shell. A path that would be a shell injection under the
 * old `system("./run.sh \"<path>\"")` call must arrive byte-for-byte.
 */
#define _POSIX_C_SOURCE 200809L

#include "launch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;

static void expect_eq(const char *what, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

int main(void) {
    char dir[] = "/tmp/nativera-launch-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }

    char script[512], capture[512], rom[512];
    snprintf(script, sizeof(script), "%s/run.sh", dir);
    snprintf(capture, sizeof(capture), "%s/got", dir);
    snprintf(rom, sizeof(rom), "%s/game; rm -rf / \"quoted'.gb", dir);

    FILE *f = fopen(script, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    /* Record exactly the argument received, then exit with a known status. */
    fprintf(f, "#!/bin/sh\nprintf '%%s' \"$1\" > \"%s\"\nexit 7\n", capture);
    fclose(f);
    if (chmod(script, 0755) != 0) {
        perror("chmod");
        return 1;
    }

    int status = -1;
    int rc = nativera_launch_game(script, rom, &status);
    expect_eq("launch returns 0", rc, 0);
    expect_eq("child exit status surfaced", status, 7);

    char got[512] = {0};
    FILE *c = fopen(capture, "r");
    if (!c) {
        fprintf(stderr, "FAIL child never wrote its argument (shell ate it?)\n");
        failures++;
    } else {
        size_t n = fread(got, 1, sizeof(got) - 1, c);
        got[n] = '\0';
        fclose(c);
        if (strcmp(got, rom) != 0) {
            fprintf(stderr, "FAIL argument mangled:\n got  '%s'\n want '%s'\n", got, rom);
            failures++;
        }
    }

    /* A program that does not exist must fail cleanly, not crash. */
    int missing = 0;
    int rc2 = nativera_launch_game("/nonexistent/nativera-run.sh", rom, &missing);
    expect_eq("missing program returns -1", rc2, -1);
    expect_eq("missing program status is -1", missing, -1);

    if (failures == 0) {
        printf("launch_test: ok\n");
    }
    return failures == 0 ? 0 : 1;
}
