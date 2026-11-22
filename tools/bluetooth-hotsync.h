#ifndef FROND_BLUETOOTH_HOTSYNC_H
#define FROND_BLUETOOTH_HOTSYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void frond_bt_sync_configure(const char* folder, const char* profile);
void frond_bt_sync_open(void);
void frond_bt_sync_close(void);
bool frond_bt_sync_input(const uint8_t* data, size_t size);
size_t frond_bt_sync_output(uint8_t* data, size_t max);

#ifdef __cplusplus
}
#endif

#endif
