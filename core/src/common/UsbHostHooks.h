#pragma once
#include <cstdint>
struct UsbHostHooks {
    void* context{nullptr};
    void (*tick)(void* context, uint64_t nowNs){nullptr};
    uint64_t (*wake)(void* context){nullptr};  // emulated ns of the next tick needed, UINT64_MAX none
    void (*replaced)(void* context){nullptr};
    void (*pinsChanged)(void*){nullptr};
};
