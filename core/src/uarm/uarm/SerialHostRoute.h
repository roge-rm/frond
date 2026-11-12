#ifndef FROND_SERIAL_HOST_ROUTE_H
#define FROND_SERIAL_HOST_ROUTE_H
#include <cstdint>

struct SerialHostHooks {
    void* context{};
    uint_fast16_t (*read)(void*){};
    void (*write)(uint_fast16_t,void*){};
    bool (*pending)(void*){};
    void (*replaced)(void*){};
    void (*modeChanged)(void*){};
};
// Board-owned UART wiring. The host endpoint never replaces the UART callbacks.
struct SerialHostRoute {
    void* context{};
    bool (*setHooks)(void*,const SerialHostHooks*){};
    bool (*enabled)(void*){};
    bool (*paused)(void*){};
    bool (*available)(void*){};
};
#endif
