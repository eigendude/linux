#ifndef _UAPI_LINUX_CARTESI_ROLLUP_H
#define _UAPI_LINUX_CARTESI_ROLLUP_H

#define CARTESI_ROLLUP_ADVANCE_STATE 0
#define CARTESI_ROLLUP_INSPECT_STATE 1

#include <linux/ioctl.h>
#include <linux/types.h>

struct rollup_bytes {
    __u8 *data;
    __u64 length;
};

struct rollup_finish {
    /* True if previous request should be accepted */
    /* False if previous request should be rejected */
    _Bool accept_previous_request;

    int next_request_type; /* either CARTESI_ROLLUP_ADVANCE or CARTESI_ROLLUP_INSPECT */
    int next_request_payload_length;
    uint8_t output_hash[32];
};

/* Finishes processing of current advance or inspect.
 * Returns only when next advance input or inspect query is ready.
 * How:
 *   Yields manual with rx-accepted if accept is true and yields manual with rx-rejected if accept is false.
 *   Once yield returns, checks the data field in fromhost to decide if next request is advance or inspect.
 *   Returns type and payload length of next request in struct
 * on success:
 *   Returns 0
 * on failure:
 *   EFAULT in case of invalid arguments
 *   ERESTARTSYS in case of an internal lock error
 *   EIO in case of yield device error
 *   EOPNOTSUPP in case of an invalid next_request_type */
#define IOCTL_ROLLUP_FINISH  _IOWR(0xd3, 0, struct rollup_finish)

/* Obtains arguments to advance state
 * How:
 *   Reads from input metadat memory range and convert data.
 *   Reads from rx buffer and copy to payload
 * on success:
 *   Returns 0
 * on failure:
 *   EOPNOTSUPP in case the driver is not currently processing an advance state
 *   EFAULT in case of invalid arguments
 *   ERESTARTSYS in case of an internal lock error
 *   EDOM in case of an integer larger than 64bits is received */
#define IOCTL_ROLLUP_READ_INPUT _IOWR(0xd3, 1, struct rollup_bytes)

/* Outputs a new output.
 *  - Copy the bytes to tx-buffer
 *  - Yields automatic with tx-output
 * on success:
 *   Returns 0
 * on failure:
 *   EOPNOTSUPP in case the driver is currently processing an inspect state
 *   EFAULT in case of invalid arguments
 *   ERESTARTSYS in case of an internal lock error
 *   EIO in case of yield device error */
#define IOCTL_ROLLUP_WRITE_OUTPUT _IOWR(0xd3, 2, struct rollup_bytes)
#endif
