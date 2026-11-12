#pragma once
#include <cstdint>
#include "UsbPacketResult.h"
struct Pxa255Udc;
bool pxa255UdcHostReset(Pxa255Udc*);
bool pxa255UdcHostFrame(Pxa255Udc*);
bool pxa255UdcHostEnabled(const Pxa255Udc*);
bool pxa255UdcHostListening(const Pxa255Udc*);
bool pxa255UdcHostSofWanted(const Pxa255Udc*);
uint32_t pxa255UdcHostEnableCount(const Pxa255Udc*);
uint8_t pxa255UdcHostAddress(const Pxa255Udc*);
void pxa255UdcHostSetNotify(Pxa255Udc*, void (*)(void*), void*);
Pxa270UdcHostResult pxa255UdcHostSetup(Pxa255Udc*, const uint8_t[8]);
Pxa270UdcHostResult pxa255UdcHostOut(Pxa255Udc*, const uint8_t*, unsigned);
Pxa270UdcHostResult pxa255UdcHostIn(Pxa255Udc*, uint8_t[16], unsigned*);
Pxa270UdcHostResult pxa255UdcHostEpOut(Pxa255Udc*, uint8_t, const uint8_t*, unsigned);
Pxa270UdcHostResult pxa255UdcHostEpIn(Pxa255Udc*, uint8_t, uint8_t[64], unsigned*);
Pxa270UdcHostResult pxa255UdcHostPeekIn(Pxa255Udc*, uint8_t, unsigned*);
struct Pxa270Udc;
bool pxa270UdcHostReset(struct Pxa270Udc *udc);
bool pxa270UdcHostFrame(struct Pxa270Udc *udc);
bool pxa270UdcHostEnabled(const struct Pxa270Udc *udc);
bool pxa270UdcHostListening(const struct Pxa270Udc *udc);
void pxa270UdcHostSetNotify(struct Pxa270Udc *udc, void (*notify)(void *), void *context);
uint32_t pxa270UdcHostEnableCount(const struct Pxa270Udc *udc);
bool pxa270UdcHostSofWanted(const struct Pxa270Udc *udc);
uint8_t pxa270UdcHostAddress(const struct Pxa270Udc *udc);
Pxa270UdcHostResult pxa270UdcHostSetup(struct Pxa270Udc *udc, const uint8_t setup[8]);
Pxa270UdcHostResult pxa270UdcHostOut(struct Pxa270Udc *udc, const uint8_t *data, unsigned size);
Pxa270UdcHostResult pxa270UdcHostIn(struct Pxa270Udc *udc, uint8_t data[16], unsigned *size);
Pxa270UdcHostResult pxa270UdcHostEpIn(struct Pxa270Udc *udc, uint8_t endpoint, uint8_t data[64],
                                      unsigned *size);
Pxa270UdcHostResult pxa270UdcHostEpOut(struct Pxa270Udc *udc, uint8_t endpoint, const uint8_t *data,
                                       unsigned size);
Pxa270UdcHostResult pxa270UdcHostPeekIn(const struct Pxa270Udc *udc, uint8_t endpoint, unsigned *size);
