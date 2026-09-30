#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include "common.h"

#define SERVICE_NAME "abs"

int main(void) {
    int coid = name_open(SERVICE_NAME, 0);
    if (coid == -1) {
        perror("name_open failed");
        return EXIT_FAILURE;
    }

    ServiceMsg req = {0};
    ServiceMsg reply;
    req.type = MSG_TYPE_REQUEST;
    snprintf(req.sender, MAX_NAME_LEN, "%s", "test_client");

    for (int i = 0; i < 10; i++) {
        if (MsgSend(coid, &req, sizeof(req), &reply, sizeof(reply)) == -1) {
            perror("MsgSend failed");
            name_close(coid);
            return EXIT_FAILURE;
        }

        if (reply.type != MSG_TYPE_REPLY || (reply.value != 0.0 && reply.value != 1.0)) {
            fprintf(stderr, "Invalid ABS response (type=%u, value=%.2f)\n",
                    reply.type, reply.value);
            name_close(coid);
            return EXIT_FAILURE;
        }

        printf("ABS decision: %s (%.0f)\n",
               reply.value == 1.0 ? "braking active" : "normal", reply.value);
        sleep(1);
    }

    name_close(coid);
    return EXIT_SUCCESS;
}
