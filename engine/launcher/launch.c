#define _POSIX_C_SOURCE 200809L

#include "launch.h"

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int nativera_spawn_wait(const char *program, char *const argv[]) {
    pid_t pid;
    int rc = posix_spawn(&pid, program, NULL, NULL, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "[launch] could not start %s: %s\n", program, strerror(rc));
        return -1;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "[launch] waitpid failed: %s\n", strerror(errno));
            return -1;
        }
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

int nativera_launch_game(const char *run_sh, const char *rom_path, int *out_status) {
    char *argv[] = {(char *)run_sh, (char *)rom_path, NULL};
    int status = nativera_spawn_wait(run_sh, argv);
    if (out_status) {
        *out_status = status;
    }
    return status < 0 ? -1 : 0;
}
