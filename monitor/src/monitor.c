#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "common.h"

#define POLL_INTERVAL_USEC 1000000 /* check every 1 second */
#define STALL_THRESHOLD 3          /* flag as stuck after 3 consecutive unchanged polls */

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

/* Open (not create) a service's shared-memory progress counter, read-only */
static uint64_t *open_progress_counter(const char *service_name) {
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%s", COUNTER_SHM_PREFIX, service_name);

    int shm_fd = shm_open(shm_name, O_RDONLY, 0666);
    if (shm_fd == -1) return NULL; /* service hasn't started / shm not created yet */

    uint64_t *counter = mmap(NULL, sizeof(uint64_t), PROT_READ, MAP_SHARED, shm_fd, 0);
    if (counter == MAP_FAILED) return NULL;

    return counter;
}

int main(void) {
    LOG_EVENT("monitor", "STARTED", "watching all services (process-state + progress-counter checks)");

    /* Process-state tracking */
    int last_state[NUM_SERVICES];

    /* Progress-counter tracking */
    uint64_t *counters[NUM_SERVICES];
    uint64_t last_counter_value[NUM_SERVICES];
    int stall_count[NUM_SERVICES];
    int reported_stuck[NUM_SERVICES];

    for (int i = 0; i < NUM_SERVICES; i++) {
        last_state[i] = -1; /* -1 = unknown yet */
        counters[i] = NULL;
        last_counter_value[i] = 0;
        stall_count[i] = 0;
        reported_stuck[i] = 0;
    }

    for (;;) {
        for (int i = 0; i < NUM_SERVICES; i++) {
            const char *name = dependency_graph[i].name;

            /* ---- Process-state check ---- */
            pid_t pid = read_service_pid(name);
            int alive = is_process_alive(pid);

            if (alive != last_state[i]) {
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

            /* ---- Progress-counter check ---- */
            if (counters[i] == NULL) {
                counters[i] = open_progress_counter(name);
            }

            if (counters[i] != NULL && alive) {
                uint64_t current = *counters[i];

                if (current == last_counter_value[i]) {
                    stall_count[i]++;
                } else {
                    stall_count[i] = 0;
                    if (reported_stuck[i]) {
                        char msg[100];
                        snprintf(msg, sizeof(msg), "%s progress resumed (counter=%llu)",
                                 name, (unsigned long long)current);
                        LOG_EVENT("monitor", "RECOVERED", msg);
                        reported_stuck[i] = 0;
                    }
                }

                if (stall_count[i] >= STALL_THRESHOLD && !reported_stuck[i]) {
                    char msg[100];
                    snprintf(msg, sizeof(msg), "%s appears STUCK (counter stalled at %llu)",
                             name, (unsigned long long)current);
                    LOG_EVENT("monitor", "FAULT_DETECTED", msg);
                    reported_stuck[i] = 1;
                }

                last_counter_value[i] = current;
            }

            if (!alive) {
                /* Process is dead — reset progress tracking so a restart starts clean */
                counters[i] = NULL;
                stall_count[i] = 0;
                reported_stuck[i] = 0;
            }
        }
        usleep(POLL_INTERVAL_USEC);
    }

    return EXIT_SUCCESS;
}