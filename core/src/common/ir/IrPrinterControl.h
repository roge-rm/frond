#ifndef FROND_IR_IRPRINTERCONTROL_H
#define FROND_IR_IRPRINTERCONTROL_H

// The host's irprinter command on a core's IR printer: status, save ABSOLUTE_PATH and clear, with
// the USB printer's status line with the bytes of a job still coming in added:
// "attached|off STATE bytes=N receiving=R last=LAST idle=MS". STATE is receiving while a job comes
// in, else ready (off when no printer). N is the held output that save writes, R the job coming in
// (not held until it ends), LAST the last save's result since the printer came on and MS the host
// milliseconds since a byte arrived or a job ended, -1 with none. Emulation thread only.

#include <chrono>
#include <string>

#include "../RawPrintFile.h"
#include "IrPrinter.h"

namespace frondIr {

class IrPrinterControl {
   public:
    // The printer came on or went off.
    void Reset() {
        last = "none";
        seen = 0;
        any = false;
    }

    // Call often (each service pass) so the idle clock starts near the change.
    void Observe(const IrPrinter* printer) {
        if (!printer || printer->Activity() == seen) return;
        seen = printer->Activity();
        any = true;
        stamp = std::chrono::steady_clock::now();
    }

    bool Command(IrPrinter* printer, const std::string& action, const std::string& argument, std::string& result) {
        Observe(printer);
        bool ok = true;
        if (action == "save") {
            if (!printer || !printer->HeldBytes()) {
                result = printer ? "Nothing to save." : "The infrared printer is off.";
                ok = false;
            } else {
                RawPrintFile output(argument);
                const auto state = printer->Save(RawPrintFile::Output, &output);
                ok = state == RawPrintJob::State::published;
                result = output.error;
                last = Name(state);
            }
        } else if (action == "clear") {
            if (printer) printer->Clear();
            else {
                result = "The infrared printer is off.";
                ok = false;
            }
        } else if (action != "status" && !action.empty()) {
            result = "Use status, save PATH or clear.";
            return false;
        }
        Observe(printer);
        if (!result.empty()) result += " ";
        result += Status(printer);
        return ok;
    }

    std::string Status(const IrPrinter* printer) const {
        if (!printer) return "off off bytes=0 receiving=0 last=" + last + " idle=-1";
        const long long idle =
            any ? std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stamp).count()
                : -1;
        return std::string("attached ") + (printer->Receiving() ? "receiving" : "ready") +
               " bytes=" + std::to_string(printer->HeldBytes()) +
               " receiving=" + std::to_string(printer->ReceivingBytes()) + " last=" + last + " idle=" + std::to_string(idle);
    }

   private:
    static const char* Name(RawPrintJob::State s) {
        static const char* names[] = {"idle",    "collecting", "publishing", "published", "rejected",
                                      "uncertain", "aborted",  "overflow",   "invalid",   "allocationFailed"};
        return names[static_cast<unsigned>(s)];
    }

    std::string last{"none"};
    uint64_t seen{0};
    bool any{false};
    std::chrono::steady_clock::time_point stamp;
};

}  // namespace frondIr

#endif
