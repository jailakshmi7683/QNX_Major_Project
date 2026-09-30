#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <time.h>
#include <stdio.h>

#define MAX_DEPS 4
#define MAX_NAME_LEN 32
#define NUM_SERVICES 4

/* Message type constants */
#define MSG_TYPE_REQUEST 1
#define MSG_TYPE_REPLY   2

typedef struct {
    char name[MAX_NAME_LEN];
    char depends_on[MAX_DEPS][MAX_NAME_LEN]; // empty string = no more deps
} ServiceNode;

static const ServiceNode dependency_graph[NUM_SERVICES] = {
    {"wheel_speed",      {"", "", "", ""}},
    {"abs",              {"wheel_speed", "", "", ""}},
    {"traction_control", {"wheel_speed", "", "", ""}},
    {"dashboard",        {"abs", "traction_control", "", ""}}
};

typedef struct {
    uint16_t type;              // MSG_TYPE_REQUEST or MSG_TYPE_REPLY
    char sender[MAX_NAME_LEN];
    double value;                // generic payload slot — reused for speed, decision, etc.
    uint64_t timestamp;          // set with ClockCycles() or clock_gettime()
} ServiceMsg;

#define LOG_EVENT(service, event, detail) \
    do { \
        struct timespec ts; \
        clock_gettime(CLOCK_REALTIME, &ts); \
        printf("[%ld.%09ld] %s | %s | %s\n", ts.tv_sec, ts.tv_nsec, service, event, detail); \
    } while (0)

#endif // COMMON_H
