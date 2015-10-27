/**
 * Test ground for developing specific tests.
 *
 * Any specific test can start of from here and set the server to the
 * specific required state, and create specific operations or reuse
 * existing ones in the test_display_base supplied queue.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "ast_base.h"

#define ASPEED_ENCODER_VIDEOCAP_DEV	"/dev/videocap"

SpiceCoreInterface *core;
#if 0
SpiceTimer *ping_timer;

int ping_ms = 100;

void pinger(SPICE_GNUC_UNUSED void *opaque)
{
    // show_channels is not thread safe - fails if disconnections / connections occur
    //show_channels(server);

    core->timer_start(ping_timer, ping_ms);
}
#endif

int main(void)
{
    Test *test;

    core = basic_event_loop_init();
    test = ast_new(core);

    test->videocap_fd = open(ASPEED_ENCODER_VIDEOCAP_DEV, O_RDONLY);
    if (test->videocap_fd < 0) {
        printf("unable to open videocap device: %d", errno);
        return -1;
    }
    test->mmap = mmap(0, 0x404000, PROT_READ, MAP_SHARED, test->videocap_fd, 0);
    if (test->mmap == MAP_FAILED) {
        close(test->videocap_fd);
        printf("unable to mmap videocap device: %d", errno);
        return -1;
    }

    bzero(&test->ioc, sizeof(ASTCap_Ioctl));
    test->ioc.OpCode = ASTCAP_IOCTL_RESET_VIDEOENGINE;
    ioctl(test->videocap_fd, ASTCAP_IOCCMD, &test->ioc);

    bzero(&test->ioc, sizeof(ASTCap_Ioctl));
    test->ioc.OpCode = ASTCAP_IOCTL_START_CAPTURE;
    ioctl(test->videocap_fd, ASTCAP_IOCCMD, &test->ioc);

//    ping_timer = core->timer_add(pinger, NULL);
//    core->timer_start(ping_timer, ping_ms);

    basic_event_loop_mainloop();

    return 0;
}
