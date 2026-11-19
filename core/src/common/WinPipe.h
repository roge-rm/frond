#pragma once
// The core's end of the launcher's control channel on Windows: a client of the named pipe Qt's
// QLocalServer listens on. Reads and writes never block. Declares the few Win32 calls itself so
// windows.h stays out of the core's sources.
#ifdef _WIN32
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <chrono>

namespace frondWinPipe {
extern "C++" {
void* __stdcall createFile(const char*, unsigned long, unsigned long, void*, unsigned long, unsigned long, void*) __asm__("CreateFileA");
int __stdcall setPipeState(void*, unsigned long*, unsigned long*, unsigned long*) __asm__("SetNamedPipeHandleState");
int __stdcall peekPipe(void*, void*, unsigned long, unsigned long*, unsigned long*, unsigned long*) __asm__("PeekNamedPipe");
int __stdcall readFile(void*, void*, unsigned long, unsigned long*, void*) __asm__("ReadFile");
int __stdcall writeFile(void*, const void*, unsigned long, unsigned long*, void*) __asm__("WriteFile");
int __stdcall closeHandle(void*) __asm__("CloseHandle");
unsigned long __stdcall lastError() __asm__("GetLastError");
}

class Client {
 public:
    Client() = default;
    ~Client() { Close(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool Open(const std::string& name) {
        const auto invalid = reinterpret_cast<void*>(~uintptr_t(0));
        for (int tries = 0; tries < 50; ++tries) {
            handle = createFile(name.c_str(), 0xC0000000ul /* read and write */, 0, nullptr, 3 /* OPEN_EXISTING */, 0, nullptr);
            if (handle != invalid) break;
            handle = nullptr;
            if (lastError() != 231 /* ERROR_PIPE_BUSY */) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!handle) return false;
        unsigned long mode = 1;  // PIPE_NOWAIT
        if (!setPipeState(handle, &mode, nullptr, nullptr)) { Close(); return false; }
        return true;
    }
    void Close() {
        if (handle) closeHandle(handle);
        handle = nullptr;
    }
    // Bytes read, 0 when the other end has gone, or -1 with errno EAGAIN when nothing has arrived.
    long Read(void* data, size_t size) {
        unsigned long available = 0;
        if (!handle || !peekPipe(handle, nullptr, 0, nullptr, &available, nullptr)) return 0;
        if (!available) { errno = EAGAIN; return -1; }
        unsigned long got = 0;
        if (!readFile(handle, data, static_cast<unsigned long>(size < available ? size : available), &got, nullptr)) return 0;
        return static_cast<long>(got);
    }
    // Bytes written (possibly fewer than size, or 0 with errno EAGAIN when the pipe is full), or -1 when the other end has gone.
    long Write(const void* data, size_t size) {
        unsigned long put = 0;
        if (!handle || !writeFile(handle, data, static_cast<unsigned long>(size), &put, nullptr)) return -1;
        if (!put && size) { errno = EAGAIN; return 0; }
        return static_cast<long>(put);
    }

 private:
    void* handle{nullptr};
};
}  // namespace frondWinPipe
#endif
