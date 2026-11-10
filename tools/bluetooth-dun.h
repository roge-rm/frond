/* The Bluetooth phone's data side for bluetooth-opp-peer's DUN mode: the RFCOMM channel's bytes go to
   a Hayes modem whose calls reach a PPP server and slirp (src/common/modem, src/common/net). A C
   interface so the C peer and BlueZ's C headers stay as they are. Single-threaded; times are
   monotonic nanoseconds. */
#ifndef FROND_BLUETOOTH_DUN_H
#define FROND_BLUETOOTH_DUN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct frond_dun;
/* Null if slirp can't start. */
struct frond_dun* frond_dun_create(void);
void frond_dun_destroy(struct frond_dun* dun);
/* The guest opened or closed the DUN channel. Closing it hangs up. */
void frond_dun_open(struct frond_dun* dun, uint64_t now);
void frond_dun_close(struct frond_dun* dun, uint64_t now);
/* Bytes the guest sent on the channel. */
void frond_dun_input(struct frond_dun* dun, const uint8_t* data, size_t size, uint64_t now);
/* Runs timers and slirp, then gives up to max bytes for the guest. */
size_t frond_dun_output(struct frond_dun* dun, uint8_t* data, size_t max, uint64_t now);
/* "idle", "dialling", "online SPEED" or "off" while the channel is closed. */
const char* frond_dun_state(const struct frond_dun* dun);
#ifdef __cplusplus
}
#endif
#endif
