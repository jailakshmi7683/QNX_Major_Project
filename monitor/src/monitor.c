#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <process.h>
#include <sys/wait.h>
#include "common.h"

#define POLL_INTERVAL_USEC      1000000  /* poll every 1 s */
#define STALL_THRESHOLD         5        /* polls; must exceed the slowest service loop (3 s) */
#define CORRELATION_WINDOW_MS   2000     /* wait for related STUCK faults to surface */
#define SAFETY_WINDOW_MS        8000     /* max time a restarted/affected service stays unmonitored */
#define MAX_RESTART_ATTEMPTS    3
#define INTER_SPAWN_DELAY_USEC  500000   /* let an upstream register its name before the next spawn */
#define SERVICE_BIN_DIR         "/tmp/"  /* where Momentics deploys the binaries on the target */

typedef enum { FAULT_NONE = 0, FAULT_DEAD, FAULT_STUCK } FaultType;

typedef struct {
    /* detection state */
    pid_t     pid;
    uint64_t *counter;
    uint64_t  last_value;
    int       stall_polls;

    /* active fault */
    FaultType fault;
    uint64_t  fault_detected_ms;
    int       symptom_logged;

    /* recovery state */
    uint64_t  grace_until_ms;       /* detection suspended until this time */
    int       recovering;           /* restarted, waiting to verify progress */
    uint64_t  restart_ms;
    uint64_t  recovery_fault_ms;    /* when the fault being recovered was detected */
    int       restart_attempts;
    int       escalated;            /* gave up after MAX_RESTART_ATTEMPTS */
} ServiceState;

static ServiceState svc[NUM_SERVICES]; /* zero-initialised: FAULT_NONE, no grace */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static const char *fault_str(FaultType t) {
    return (t == FAULT_DEAD) ? "dead" : "stuck";
}

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

static int is_process_alive(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    if (errno == ESRCH) return 0;
    return 1;
}

static uint64_t *open_progress_counter(const char *service_name) {
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%s", COUNTER_SHM_PREFIX, service_name);

    int shm_fd = shm_open(shm_name, O_RDONLY, 0666);
    if (shm_fd == -1) return NULL;

    uint64_t *counter = mmap(NULL, sizeof(uint64_t), PROT_READ, MAP_SHARED, shm_fd, 0);
    if (counter == MAP_FAILED) return NULL;
    return counter;
}

/* ------------------------------------------------------------------ */
/* Dependency graph queries                                            */
/* ------------------------------------------------------------------ */

static int service_index(const char *name) {
    for (int i = 0; i < NUM_SERVICES; i++) {
        if (strcmp(dependency_graph[i].name, name) == 0) return i;
    }
    return -1;
}

/* Returns 1 if service 'anc' is (transitively) upstream of service 'desc'. */
static int is_ancestor(int anc, int desc, int depth) {
    if (depth > NUM_SERVICES) return 0; /* guards against accidental cycles */

    for (int k = 0; k < MAX_DEPS; k++) {
        const char *dep = dependency_graph[desc].depends_on[k];
        if (dep[0] == '\0') break;

        int p = service_index(dep);
        if (p < 0) continue;
        if (p == anc) return 1;
        if (is_ancestor(anc, p, depth + 1)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Detection: process-state check + progress-counter check            */
/* ------------------------------------------------------------------ */

static void detect_faults(int i, uint64_t now) {
    ServiceState *s = &svc[i];
    const char *name = dependency_graph[i].name;
    char msg[200];

    if (now < s->grace_until_ms) return; /* recovery window: detection suspended */

    /* ---- process-state check ---- */
    s->pid = read_service_pid(name);
    int alive = is_process_alive(s->pid);

    if (!alive) {
        if (s->fault != FAULT_DEAD) {
            s->fault = FAULT_DEAD;
            s->fault_detected_ms = now;
            s->symptom_logged = 0;
            s->stall_polls = 0;
            snprintf(msg, sizeof(msg), "%s appears DEAD (pid=%d)", name, (int)s->pid);
            LOG_EVENT("monitor", "FAULT_DETECTED", msg);
        }
        return;
    }

    if (s->fault == FAULT_DEAD) { /* it came back without our help */
        s->fault = FAULT_NONE;
        snprintf(msg, sizeof(msg), "%s is back UP (pid=%d)", name, (int)s->pid);
        LOG_EVENT("monitor", "PROCESS_UP", msg);
    }

    /* ---- progress-counter check ---- */
    if (s->counter == NULL) {
        s->counter = open_progress_counter(name);
        if (s->counter) s->last_value = *s->counter;
        s->stall_polls = 0;
        return;
    }

    uint64_t cur = *s->counter;
    if (cur != s->last_value) {
        s->last_value = cur;
        s->stall_polls = 0;
        if (s->fault == FAULT_STUCK) {
            s->fault = FAULT_NONE;
            snprintf(msg, sizeof(msg), "%s progress resumed on its own (counter=%llu)",
                     name, (unsigned long long)cur);
            LOG_EVENT("monitor", "PROGRESS_RESUMED", msg);
        }
    } else {
        s->stall_polls++;
    }

    if (s->stall_polls >= STALL_THRESHOLD && s->fault == FAULT_NONE) {
        s->fault = FAULT_STUCK;
        s->fault_detected_ms = now;
        s->symptom_logged = 0;
        snprintf(msg, sizeof(msg), "%s appears STUCK (counter stalled at %llu)",
                 name, (unsigned long long)cur);
        LOG_EVENT("monitor", "FAULT_DETECTED", msg);
    }
}

/* ------------------------------------------------------------------ */
/* Recovery                                                            */
/* ------------------------------------------------------------------ */

static void restart_service(int i, FaultType ft, uint64_t now) {
    ServiceState *s = &svc[i];
    const char *name = dependency_graph[i].name;
    char msg[200];

    if (s->restart_attempts >= MAX_RESTART_ATTEMPTS) {
        snprintf(msg, sizeof(msg),
                 "%s failed %d recovery attempts - ESCALATING to safe state, no further restarts",
                 name, s->restart_attempts);
        LOG_EVENT("monitor", "ESCALATE", msg);
        s->escalated = 1;
        return;
    }
    s->restart_attempts++;

    snprintf(msg, sizeof(msg), "%s is ROOT CAUSE (%s) - restarting (attempt %d/%d)",
             name, fault_str(ft), s->restart_attempts, MAX_RESTART_ATTEMPTS);
    LOG_EVENT("monitor", "ROOT_CAUSE", msg);

    /* A hung process is still alive: kill it before starting a fresh copy */
    if (ft == FAULT_STUCK && s->pid > 0) {
        kill(s->pid, SIGKILL);
        for (int w = 0; w < 20; w++) { /* wait up to ~1 s for it to disappear */
            waitpid(s->pid, NULL, WNOHANG);
            if (!is_process_alive(s->pid)) break;
            usleep(50000);
        }
    }

    char path[64];
    snprintf(path, sizeof(path), "%s%s", SERVICE_BIN_DIR, name);
    pid_t newpid = spawnl(P_NOWAIT, path, path, (char *)NULL);
    if (newpid == -1) {
        snprintf(msg, sizeof(msg), "could not spawn %s (check the binary exists at that path)", path);
        LOG_EVENT("monitor", "RESTART_FAILED", msg);
        return; /* fault stays active; retried on the next poll, counts toward attempts */
    }

    snprintf(msg, sizeof(msg), "%s restarted (new pid=%d)", name, (int)newpid);
    LOG_EVENT("monitor", "RESTARTED", msg);

    s->recovering = 1;
    s->restart_ms = now;
    s->recovery_fault_ms = s->fault_detected_ms;
    s->last_value = 0;

    /* Suspend detection for the restarted service and everything downstream of it,
       bounded by SAFETY_WINDOW_MS. Faults on descendants were symptoms: clear them. */
    for (int k = 0; k < NUM_SERVICES; k++) {
        if (k == i || is_ancestor(i, k, 0)) {
            svc[k].grace_until_ms = now + SAFETY_WINDOW_MS;
            svc[k].stall_polls = 0;
            svc[k].fault = FAULT_NONE;
            svc[k].symptom_logged = 0;
        }
    }

    usleep(INTER_SPAWN_DELAY_USEC);
}

/* After a restart: confirm the service is making progress again, or retry. */
static void check_recovery(int i, uint64_t now) {
    ServiceState *s = &svc[i];
    const char *name = dependency_graph[i].name;
    char msg[220];

    if (!s->recovering) return;

    if (s->counter == NULL) s->counter = open_progress_counter(name);
    int alive = is_process_alive(read_service_pid(name));

    if (alive && s->counter && *s->counter > 0) {
        snprintf(msg, sizeof(msg),
                 "%s healthy again: recovery_time=%llu ms (fault detected -> progress), "
                 "restart_to_progress=%llu ms, within_safety_window=%s",
                 name,
                 (unsigned long long)(now - s->recovery_fault_ms),
                 (unsigned long long)(now - s->restart_ms),
                 (now - s->restart_ms) <= SAFETY_WINDOW_MS ? "yes" : "no");
        LOG_EVENT("monitor", "RECOVERY_VERIFIED", msg);

        s->recovering = 0;
        s->restart_attempts = 0;
        s->escalated = 0;
        s->last_value = *s->counter;
        s->stall_polls = 0;
    } else if (now >= s->restart_ms + SAFETY_WINDOW_MS) {
        snprintf(msg, sizeof(msg),
                 "%s showed no progress within %d ms of restart - safety window exceeded",
                 name, SAFETY_WINDOW_MS);
        LOG_EVENT("monitor", "SAFETY_WINDOW_EXCEEDED", msg);

        s->recovering = 0;
        s->fault = alive ? FAULT_STUCK : FAULT_DEAD;
        s->fault_detected_ms = now - CORRELATION_WINDOW_MS; /* ready to act immediately */
        s->symptom_logged = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Root-cause isolation                                                */
/* ------------------------------------------------------------------ */

static void isolate_and_recover(uint64_t now) {
    int restart[NUM_SERVICES] = {0};
    FaultType ftype[NUM_SERVICES];
    char msg[200];

    for (int i = 0; i < NUM_SERVICES; i++) {
        ServiceState *s = &svc[i];
        ftype[i] = s->fault;

        if (s->fault == FAULT_NONE || s->escalated) continue;

        /* STUCK is ambiguous: give related faults a moment to surface first.
           DEAD is unambiguous (a dead process can't be "waiting"): act now. */
        if (s->fault == FAULT_STUCK && now - s->fault_detected_ms < CORRELATION_WINDOW_MS) continue;

        int cause = -1;
        if (s->fault == FAULT_STUCK) {
            for (int j = 0; j < NUM_SERVICES; j++) {
                if (j == i || svc[j].fault == FAULT_NONE) continue;
                if (!is_ancestor(j, i, 0)) continue;
                /* upstream fault detected earlier, or near-concurrently => this is a symptom */
                if (svc[j].fault_detected_ms <= s->fault_detected_ms + CORRELATION_WINDOW_MS) {
                    cause = j;
                    break;
                }
            }
        }

        if (cause >= 0) {
            if (!s->symptom_logged) {
                snprintf(msg, sizeof(msg),
                         "%s is a SYMPTOM of upstream %s - not restarting",
                         dependency_graph[i].name, dependency_graph[cause].name);
                LOG_EVENT("monitor", "SYMPTOM", msg);
                s->symptom_logged = 1;
            }
            continue;
        }

        restart[i] = 1;
    }

    /* Ascending index order is upstream-first because dependency_graph[] is listed
       upstream-first. Keep it that way if you ever reorder the table. */
    for (int i = 0; i < NUM_SERVICES; i++) {
        if (restart[i]) restart_service(i, ftype[i], now);
    }
}

/* ------------------------------------------------------------------ */

int main(void) {
    LOG_EVENT("monitor", "STARTED",
              "process-state + progress-counter detection, root-cause isolation, selective recovery");

    for (;;) {
        uint64_t now = now_ms();

        while (waitpid(-1, NULL, WNOHANG) > 0) { } /* reap services we spawned that have died */

        for (int i = 0; i < NUM_SERVICES; i++) detect_faults(i, now);
        for (int i = 0; i < NUM_SERVICES; i++) check_recovery(i, now);
        isolate_and_recover(now);

        usleep(POLL_INTERVAL_USEC);
    }

    return EXIT_SUCCESS;
}
