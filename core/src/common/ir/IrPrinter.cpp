#include "IrPrinter.h"

namespace frondIr {

IrLap::Options IrPrinter::PrinterLap() {
    IrLap::Options options;
    options.address = {{0x46, 0x72, 0x50, 0x72}};
    // Printer with the extension bit, then IrCOMM (IrLMP 1.1 section 3.4.1).
    options.hints = {{0x88, 0x04}};
    options.nickname = "Frond Printer";
    // A guest restored in the middle of a print polls a link the new printer never had, or holds one
    // it never sends on. The printer stays quiet and drops a link that never carried print data, so
    // the guest's disconnect time ends it and the next Print connects afresh.
    options.answerDm = false;
    options.idleNs = 20000000000ull;
    return options;
}

IrPrinter::IrPrinter() : IrPrinter(Options{}) {}

IrPrinter::IrPrinter(Options options)
    : lmp(lap),
      lap(lmp, options.lap),
      ias(lmp),
      lpt(*this),
      comm(lmp, ias, *this, options.lap.nickname),
      job(options.limit),
      held(options.limit) {
    lmp.Bind(lptLsap, lpt);
    ias.AddInteger("IrLPT", "IrDA:IrLMP:LsapSel", lptLsap);
    held.begin();
}

void IrPrinter::Drop(uint64_t now) {
    if (job.abort()) {
        ++stats.dropped;
        ++activity;
    }
    lap.Drop(now);
}

void IrPrinter::Begin() {
    // One job at a time: a new connection ends the one before.
    End();
    job.begin();
    ++activity;
}

void IrPrinter::Append(const uint8_t* data, size_t size) {
    if (!size || !Receiving()) return;
    lap.LinkBusy();
    if (!job.append(data, size)) {
        ++stats.overflows;
        // The guest is told the printer has gone, rather than sending on into nothing, on
        // IrLPT and on either IrCOMM LSAP.
        for (const uint8_t lsap : {lptLsap, IrComm::cookedLsap, IrComm::rawLsap})
            if (lmp.Connected(lsap)) lmp.Disconnect(lsap, IrLmp::userRequest, 0);
    }
    ++activity;
}

void IrPrinter::End() {
    if (!Receiving()) return;
    ++activity;
    if (job.size() > held.limit() - held.size()) {
        job.abort();
        ++stats.overflows;
        return;
    }
    struct Into {
        static RawPrintJob::Publication Append(void* context, const uint8_t* data, size_t size) {
            return static_cast<RawPrintJob*>(context)->append(data, size) ? RawPrintJob::Publication::published
                                                                          : RawPrintJob::Publication::rejected;
        }
    };
    if (job.finish(Into::Append, &held) == RawPrintJob::State::published) {
        ++heldJobs;
        ++stats.jobs;
    } else {
        // Allocation failed: held lost its bytes too, so it starts again empty.
        heldJobs = 0;
        held.begin();
    }
}

RawPrintJob::State IrPrinter::Save(RawPrintJob::Output output, void* context) {
    if (!held.size()) return RawPrintJob::State::rejected;
    const auto result = held.finish(output, context);
    heldJobs = 0;
    held.begin();
    ++activity;
    return result;
}

void IrPrinter::Clear() {
    held.abort();
    heldJobs = 0;
    held.begin();
    ++activity;
}

std::string IrPrinter::Describe() const {
    if (Receiving()) return "printer receiving";
    return lap.Connected() ? "printer linked" : "printer idle";
}

bool IrPrinter::Lpt::Connect(const Bytes& data, Bytes& reply, uint64_t now) {
    printer.Begin();
    return true;
}

void IrPrinter::Lpt::Data(const Bytes& data, uint64_t now) { printer.Append(data.data(), data.size()); }

void IrPrinter::Lpt::Disconnect(uint64_t now) { printer.End(); }

}  // namespace frondIr
