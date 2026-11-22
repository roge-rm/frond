#ifndef FROND_BLUETOOTH_RFCOMM_STREAM_H
#define FROND_BLUETOOTH_RFCOMM_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FROND_RFCOMM_MAX_FRAME 667

struct bthost;

// A single inbound serial service. Space is the payload size of the next frame, or zero while blocked.
void frond_rfcomm_stream_enable(struct bthost* host, uint8_t channel,
                               void (*disc)(uint16_t handle, uint16_t cid));
size_t frond_rfcomm_stream_space(struct bthost* host, uint16_t handle, uint16_t cid);
bool frond_rfcomm_stream_send(struct bthost* host, uint16_t handle, uint16_t cid,
                             const void* data, uint16_t size);

#endif
