#include <stdint.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/cartesi/rollup.h>
#include "../../kselftest_harness.h"

#define ROLLUP_DEVICE_NAME "/dev/rollup"


FIXTURE(rollup) {
    int fd;
};
FIXTURE_SETUP(rollup) {
    self->fd = open(ROLLUP_DEVICE_NAME, O_RDWR);
    ASSERT_GT(self->fd, 0) {
        TH_LOG("fixture error: %s\n", strerror(self->fd));
    }
}
FIXTURE_TEARDOWN(rollup) {
    close(self->fd);
}
TEST_F(rollup, echo) {
    int ret = 0;

    struct rollup_finish finish = {
        .accept_previous_request = true,
    };
    static uint8_t buf[4096];
    struct rollup_bytes rx = {buf, sizeof(buf)};

    ASSERT_EQ(ioctl(self->fd, IOCTL_ROLLUP_FINISH, (unsigned long) &finish), 0);
    ASSERT_EQ(finish.next_request_type, CARTESI_ROLLUP_ADVANCE_STATE);
    //ASSERT_EQ(finish.next_request_payload_length, 1024); // make it so the test writes this value

    // read input
    int rc = ioctl(self->fd, IOCTL_ROLLUP_READ_INPUT, (unsigned long) &rx);
    if (rc) printf("%s\n", strerror(errno));
    ASSERT_EQ(rc, 0);

    // echo it back
    ASSERT_EQ(ioctl(self->fd, IOCTL_ROLLUP_WRITE_OUTPUT, (unsigned long) &rx), 0);

    // echo it back twice
    ASSERT_EQ(ioctl(self->fd, IOCTL_ROLLUP_WRITE_OUTPUT, (unsigned long) &rx), 0);

    // TODO: update the hash
    // we are done
    ASSERT_EQ(ioctl(self->fd, IOCTL_ROLLUP_FINISH, (unsigned long) &finish), 0);
}

TEST_HARNESS_MAIN
