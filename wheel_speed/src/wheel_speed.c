#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/types.h>
#include "common.h"


#define SERVICE_NAME "wheel_speed"





int main(void) {
    name_attach_t *attach;
    ServiceMsg msg;
    double t = 0.0;

    /* Register this process under a well-known name so clients can find it */
    attach = name_attach(NULL, SERVICE_NAME, 0);
    if (attach == NULL) {
        perror("name_attach failed");
        exit(EXIT_FAILURE);
    }

    LOG_EVENT(SERVICE_NAME, "STARTED", "waiting for requests");
    FILE *pidf = fopen("/tmp/" SERVICE_NAME ".pid", "w");
    if (pidf) {
        fprintf(pidf, "%d", getpid());
        fclose(pidf);
    }

    for (;;) {
        /* Block until a client sends a request */
        int rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);

        if (rcvid < 0) {
            /* Error receiving — log and keep looping */
            LOG_EVENT(SERVICE_NAME, "ERROR", "MsgReceive failed");
            continue;
        }

        if (rcvid == 0) {
            /* This was a pulse, not a message — ignore for now */
            continue;
        }

        if (msg.type == MSG_TYPE_REQUEST) {
            /* Generate a fake wheel speed value: a slowly varying number */
            t += 0.1;
            double fake_speed = 60.0 + 10.0 * sin(t); /* oscillates around 60 km/h */

            ServiceMsg reply;
            reply.type = MSG_TYPE_REPLY;
            snprintf(reply.sender, MAX_NAME_LEN, "%s", SERVICE_NAME);
            reply.value = fake_speed;
            reply.timestamp = (uint64_t)ClockCycles();

            MsgReply(rcvid, EOK, &reply, sizeof(reply));

            char log_msg[100];

            snprintf(log_msg, sizeof(log_msg),
                     "sent speed value = %.2f",
                     fake_speed);

            LOG_EVENT(SERVICE_NAME, "REPLIED", log_msg);
        } else {
            /* Unknown message type — reply with an error */
            MsgError(rcvid, EBADMSG);
        }
    }

    name_detach(attach, 0);
    return EXIT_SUCCESS;
}
