//#include <stdio.h>
//#include <stdlib.h>
//
//int main(void) {
//	puts("Hello World!!!"); /* prints Hello World!!! */
//	return EXIT_SUCCESS;
//}

#include <stdio.h>
#include <sys/neutrino.h>
#include "common.h"

int main(void) {
    int coid = name_open("wheel_speed", 0);
    if (coid == -1) {
        perror("name_open failed");
        return 1;
    }

    ServiceMsg req, reply;
    req.type = MSG_TYPE_REQUEST;
    snprintf(req.sender, MAX_NAME_LEN, "test_client");

    for (int i = 0; i < 5; i++) {
        MsgSend(coid, &req, sizeof(req), &reply, sizeof(reply));
        printf("Got speed: %.2f\n", reply.value);
        sleep(1);
    }

    name_close(coid);
    return 0;
}
