#ifndef _UART_BCM2035_H_
#define _UART_BCM2035_H_

#include <array>
#include <cstdint>
#include <string>
#include <functional>
struct PxaGpio;

#include "get_emu_time.h"
#include "pxa_UART.h"

struct Bcm2035;

struct Bcm2035* bcm2035Init(GetEmuTime getTime,PxaGpio* gpio=nullptr,bool tx=false);

void bcm2035RegisterWithUart(struct Bcm2035* bcm2035, PxaUart* uart);

template <typename T>
void bcm2035Save(struct Bcm2035* bcm2035, T& savestate);

template <typename T>
void bcm2035Load(struct Bcm2035* bcm2035, T& loader);

bool bcm2035SetBackend(Bcm2035*,const std::string& path,std::string& error);
// The machine's controller address, programmed on every controller connection.
void bcm2035SetAddress(Bcm2035*,const std::array<uint8_t,6>&);
// False until an address is set or the guest programs one.
bool bcm2035Address(Bcm2035*,std::array<uint8_t,6>&);
std::string bcm2035BackendState(Bcm2035*);
// Host-only deferred GPIO history, restored before tentative GPIO loads.
std::function<void()> bcm2035CaptureInput(Bcm2035*);
void bcm2035BeginCheckpoint(Bcm2035*);
void bcm2035EndCheckpoint(Bcm2035*,bool sleeping=false);
void bcm2035Suspend(Bcm2035*);
void bcm2035PowerChanged(Bcm2035*);
void bcm2035Stop(Bcm2035*);

#endif  // _UART_BCM2035_H_
