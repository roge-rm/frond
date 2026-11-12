#ifndef FROND_ARM_DIAGNOSTIC_H
#define FROND_ARM_DIAGNOSTIC_H
#include <cstdint>
// Private diagnostic records. Borrowed arrays are valid only during each callback.
// Callbacks must not re-enter the CPU or change guest state. No restore format.
struct ArmDiagnosticSink {
    void* context;
    void (*words)(void*, const char*, const uint32_t*, uint32_t);
    void (*bytes)(void*, const char*, const uint8_t*, uint32_t);
    void emit(const char* name, const uint32_t* data, uint32_t count) const {
        if (words) words(context, name, data, count);
    }
};
#endif
