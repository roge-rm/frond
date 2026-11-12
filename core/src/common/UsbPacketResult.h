#pragma once
// Timeout is a token with no response, distinct from a NAK handshake.
enum class UsbPacketResult { Ack, Nak, Stall, Disabled, Unsupported, Timeout };
using Pxa270UdcHostResult = UsbPacketResult;
