#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "common.h"

#define POLL_INTERVAL_USEC 1000000 /* check every 1 second */

/* Read a service's PID from its pid file. Returns -1 if unreadable. */
static pid_t read_service_pid(const char *service_name) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/%s.pid", service_name);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    pid_t pid;
    int ok = fscanf(f, "%d", &pid);
    fclose(f);

    return (ok == 1) ? pid : -1;
}

/* Process-state check: is this PID still alive? */
static int is_process_alive(pid_t pid) {
    if (pid <= 0) return 0;
    /* kill(pid, 0) sends no actual signal — it just checks existence/permission */
    if (kill(pid, 0) == 0) return 1;
    if (errno == ESRCH) return 0; /* no such process — genuinely dead */
    return 1; /* some other error (e.g. permission) — assume alive, don't false-positive */
}

int main(void) {
    LOG_EVENT("monitor", "STARTED", "watching all services (process-state check)");

    /* Track last-known alive/dead state per service to only log on change */
    int last_state[NUM_SERVICES];
    for (int i = 0; i < NUM_SERVICES; i++) last_state[i] = -1; /* -1 = unknown yet */

    for (;;) {
        for (int i = 0; i < NUM_SERVICES; i++) {
            const char *name = dependency_graph[i].name;
            pid_t pid = read_service_pid(name);
            int alive = is_process_alive(pid);

            if (alive != last_state[i]) {
                char detail[64];
                snprintf(detail, sizeof(detail), "pid=%d", pid);
                LOG_EVENT("monitor", alive ? "PROCESS_UP" : "PROCESS_DOWN", detail);
                /* prefix the service name into the detail so you know who this is about */
                if (!alive) {
                    char msg[100];
                    snprintf(msg, sizeof(msg), "%s appears DEAD (pid=%d)", name, pid);
                    LOG_EVENT("monitor", "FAULT_DETECTED", msg);
                } else if (last_state[i] == 0) {
                    char msg[100];
                    snprintf(msg, sizeof(msg), "%s is back UP (pid=%d)", name, pid);
                    LOG_EVENT("monitor", "RECOVERED", msg);
                }
                last_state[i] = alive;
            }
        }
        usleep(POLL_INTERVAL_USEC);
    }

    return EXIT_SUCCESS;
}
