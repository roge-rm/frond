#pragma once
// The Dana's USB printer in the desktop app: the core's status line, when a job counts as
// received, and drawing PrintText pages on a printer or PDF.
#include <QString>
#include <QtGlobal>
#include "PrintText.h"

class QPrinter;

namespace frondPrint {

// "[error text ]attached|off STATE bytes=N last=LAST idle=MS"; valid is false for anything else.
struct Status {
    bool valid{false}, attached{false};
    QString state, last, error;
    qint64 bytes{0}, idleMs{-1};
};
Status parseStatus(const QString& message);

// "off", "printer STATUS" or "wifi powered|unpowered link=up|down ip=ADDRESS ..." from usb-port-state.
struct UsbPort {
    bool valid{false}, powered{false}, linkUp{false};
    QString port, ip;
};
UsbPort parseUsbPort(const QString& message);
// The Wi-Fi adapter's line in the Connections panel.
QString wifiReadout(const UsbPort& state);

// A job counts as received when bytes have come in and then stopped for this long.
constexpr qint64 pauseMs = 3000;
inline bool received(const Status& s) { return s.valid && s.attached && s.bytes > 0 && s.idleMs >= pauseMs; }

// One PrintText page per paper page, in a monospaced font sized so 80 columns by 66 lines fit
// the page. Returns the number of pages drawn, 0 if the printer couldn't be painted on.
int drawPages(QPrinter* printer, const frond::PrintText& text);

}  // namespace frondPrint
