// The IR printer: a scripted IrLAP primary, written from the IrDA IrLAP 1.1, IrLMP 1.1, Tiny TP 1.1,
// IrCOMM 1.0 and IrLPT specs, prints as PrintBoy's IrMgr does: discovery, SNRM, IAS for class
// "IrLPT" and its IrLMP LSAP, an IrLMP connect, the data, then an LM disconnect. Each job must
// come out as exactly the bytes sent. Also DISC, the watchdog, a load,
// IrCOMM, overflow, clear and save results.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unistd.h>
#include <string>
#include <vector>

#include "ir/IrPrinter.h"
#include "ir/IrPrinterControl.h"
#include "ir/Sir.h"

using namespace frondIr;

static unsigned checks, failures;
#define CHECK(condition)                                                            \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(condition)) {                                                         \
            ++failures;                                                             \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                           \
    } while (0)

constexpr uint64_t ms = 1000000ull;

static Bytes Cat(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

static Bytes Text(const std::string& text) { return Bytes(text.begin(), text.end()); }

// Everything an output callback was given, and what it answers.
struct Capture {
    std::vector<Bytes> outputs;
    RawPrintJob::Publication answer{RawPrintJob::Publication::published};
    static RawPrintJob::Publication Output(void* context, const uint8_t* data, size_t size) {
        auto& c = *static_cast<Capture*>(context);
        c.outputs.emplace_back(data, data + size);
        return c.answer;
    }
};

// ---- A scripted primary ----

struct Primary {
    IrPrinter& printer;
    uint64_t now{1000 * ms};
    SirUnwrapper unwrapper;
    Bytes address{0x31, 0x41, 0x59, 0x26};
    uint8_t caddr{0x5a};
    uint8_t vs{0}, vr{0};
    uint8_t iasLsap{0x01}, lptPeer{0x0a}, commLsap{0x05};
    // LM-PDUs from the printer.
    std::vector<Bytes> pdus;

    explicit Primary(IrPrinter& printer) : printer(printer) {}

    void Send(const Bytes& frame, unsigned extraBofs = 0) {
        std::vector<uint8_t> wire;
        SirWrap(frame, extraBofs, wire);
        printer.Write(wire.data(), wire.size(), now);
    }

    std::vector<Bytes> Collect(uint64_t limit = 600 * ms, bool untilFinal = true) {
        std::vector<Bytes> frames;
        for (uint64_t waited = 0; waited < limit; waited += ms) {
            now += ms;
            printer.Service(now);
            uint8_t data[512];
            size_t n;
            bool final = false;
            while ((n = printer.Read(data, sizeof(data), now)) > 0)
                for (size_t i = 0; i < n; ++i)
                    if (unwrapper.Feed(data[i])) {
                        frames.push_back(unwrapper.Frame());
                        final = final || (unwrapper.Frame().size() >= 2 && (unwrapper.Frame()[1] & 0x10));
                    }
            if (final && untilFinal) break;
        }
        return frames;
    }

    void Idle(uint64_t span) {
        for (uint64_t t = 0; t < span; t += ms) {
            now += ms;
            printer.Service(now);
        }
    }

    Bytes Command(uint8_t control, const Bytes& info = {}) { return Cat({uint8_t(caddr | 1), control}, info); }

    std::vector<Bytes> Exchange(const Bytes& info) {
        Send(Command(uint8_t(vr << 5 | 0x10 | vs << 1), info));
        vs = (vs + 1) & 7;
        return Take(Collect());
    }

    std::vector<Bytes> Take(const std::vector<Bytes>& frames) {
        for (const Bytes& f : frames) {
            if (f.size() < 2 || (f[1] & 1)) continue;
            if (((f[1] >> 1) & 7) != vr) continue;
            vr = (vr + 1) & 7;
            pdus.emplace_back(f.begin() + 2, f.end());
        }
        return frames;
    }
};

static Bytes Discovery(const Bytes& from, uint8_t flags, uint8_t slot) {
    Bytes info{0x01};
    info = Cat(Cat(info, from), {0xff, 0xff, 0xff, 0xff, flags, slot, 0x00});
    if (slot == 0xff) info = Cat(Cat(info, {0x82, 0x04, 0x00}), Text("Palm"));
    return Cat({0xff, 0x3f}, info);
}

static const Bytes printerAddress{0x46, 0x72, 0x50, 0x72};

static Bytes Snrm(Primary& p, uint8_t disconnect = 0xff) {
    Bytes info = Cat(p.address, printerAddress);
    info.push_back(p.caddr);
    info = Cat(info, {0x01, 0x01, 0x3f, 0x82, 0x01, 0x01, 0x83, 0x01, 0x0f, 0x84, 0x01, 0x01, 0x85, 0x01, 0x08, 0x86,
                      0x01, 0x07, 0x08, 0x01, disconnect});
    return Cat({0xff, 0x93}, info);
}

// Discovery, SNRM and UA, and the first poll; returns the discovery answers.
static std::vector<Bytes> Connect(Primary& p, uint8_t disconnect = 0xff) {
    std::vector<Bytes> answers;
    for (uint8_t slot = 0; slot < 6; ++slot) {
        p.Send(Discovery(p.address, 0x01, slot), 10);
        for (const Bytes& f : p.Collect(60 * ms, false)) answers.push_back(f);
    }
    p.Send(Discovery(p.address, 0x01, 0xff), 10);
    p.Idle(5 * ms);
    p.Send(Snrm(p, disconnect), 10);
    p.Collect();
    p.vs = p.vr = 0;
    p.Send(p.Command(0x11));
    p.Take(p.Collect());
    return answers;
}

static Bytes LmConnect(Primary& p, uint8_t from, uint8_t to, const Bytes& user) {
    const auto frames = p.Exchange(Cat({uint8_t(0x80 | to), from, 0x01, 0x00}, user));
    for (const Bytes& f : frames)
        if (f.size() >= 6 && !(f[1] & 1) && f[2] == (0x80 | from) && f[3] == to && f[4] == 0x81)
            return Bytes(f.begin() + 6, f.end());
    return {0xee};
}

static Bytes Ias(Primary& p, const std::string& className, const std::string& attribute) {
    Bytes request{0x00, p.iasLsap, 0x84, uint8_t(className.size())};
    request = Cat(Cat(request, Text(className)), {uint8_t(attribute.size())});
    request = Cat(request, Text(attribute));
    for (const Bytes& f : p.Exchange(request))
        if (f.size() >= 5 && !(f[1] & 1) && f[2] == p.iasLsap && f[3] == 0x00 && f[4] == 0x84)
            return Bytes(f.begin() + 5, f.end());
    return {0xee};
}

// Print data on the IrLPT connection, in frames of at most 64 bytes.
static void SendLpt(Primary& p, const Bytes& data) {
    for (size_t at = 0; at < data.size(); at += 64) {
        const size_t n = std::min<size_t>(64, data.size() - at);
        p.Exchange(Cat({IrPrinter::lptLsap, p.lptPeer}, Bytes(data.begin() + at, data.begin() + at + n)));
    }
}

static Bytes Job(size_t size, uint8_t seed) {
    Bytes job(size);
    for (size_t i = 0; i < size; ++i) job[i] = uint8_t(seed + i * 7);
    // Binary zeros and the SIR control bytes are print data like any other.
    if (size > 4) job[1] = 0x00, job[2] = 0xc0, job[3] = 0x7d, job[4] = 0xc1;
    return job;
}

// ---- PrintBoy's IrLPT job ----

static void TestLptJob() {
    IrPrinter printer;
    Primary p{printer};
    CHECK(!printer.NeedsGuest(p.now));

    const auto answers = Connect(p);
    CHECK(answers.size() == 1);
    if (answers.size() == 1) {
        const Bytes& x = answers[0];
        CHECK(x.size() > 14 && Bytes(x.begin() + 14, x.end()) == Cat({0x88, 0x04, 0x00}, Text("Frond Printer")));
    }
    CHECK(printer.Lap().Connected());
    CHECK(printer.Describe() == "printer linked");
    // A linked printer keeps the guest running in the background, so the guest can end the job.
    CHECK(printer.NeedsGuest(p.now));

    // IrMgr's query, then the IrCOMM classes are there as well.
    CHECK(LmConnect(p, p.iasLsap, 0x00, {}).empty());
    CHECK(Ias(p, "IrLPT", "IrDA:IrLMP:LsapSel") ==
          Bytes({0x00, 0x00, 0x01, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, IrPrinter::lptLsap}));
    CHECK(Ias(p, "IrDA:IrCOMM", "IrDA:TinyTP:LsapSel") ==
          Bytes({0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, IrComm::cookedLsap}));
    CHECK(Ias(p, "Device", "DeviceName") == Cat({0x00, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 13}, Text("Frond Printer")));
    p.Exchange({0x80, p.iasLsap, 0x02, 0x01});

    // IrLMP connect with no user data, the job, then the LM disconnect ends it.
    const Bytes job = Job(1000, 3);
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    CHECK(printer.Receiving() && printer.Describe() == "printer receiving");
    const uint64_t before = printer.Activity();
    SendLpt(p, job);
    CHECK(printer.Activity() != before);
    CHECK(printer.ReceivingBytes() == job.size() && printer.HeldBytes() == 0);
    p.Exchange({uint8_t(0x80 | IrPrinter::lptLsap), p.lptPeer, 0x02, 0x01});
    CHECK(!printer.Receiving());
    CHECK(printer.HeldBytes() == job.size() && printer.HeldJobs() == 1);

    // A second job joins the held output; the IrLAP link stays up between.
    const Bytes second = Text("Second page\r\n\f");
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    SendLpt(p, second);
    p.Exchange({uint8_t(0x80 | IrPrinter::lptLsap), p.lptPeer, 0x02, 0x01});
    CHECK(printer.HeldJobs() == 2 && printer.GetStats().jobs == 2);

    // Save gives exactly those bytes once and empties the held output.
    Capture capture;
    CHECK(printer.Save(Capture::Output, &capture) == RawPrintJob::State::published);
    CHECK(capture.outputs.size() == 1 && capture.outputs[0] == Cat(job, second));
    CHECK(printer.HeldBytes() == 0 && printer.HeldJobs() == 0);
    CHECK(printer.Save(Capture::Output, &capture) == RawPrintJob::State::rejected);
    CHECK(capture.outputs.size() == 1);

    // A failed or uncertain save still empties it: the core never retries.
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    SendLpt(p, Text("x"));
    p.Exchange({uint8_t(0x80 | IrPrinter::lptLsap), p.lptPeer, 0x02, 0x01});
    capture.answer = RawPrintJob::Publication::uncertain;
    CHECK(printer.Save(Capture::Output, &capture) == RawPrintJob::State::uncertain);
    CHECK(printer.HeldBytes() == 0);

    // A connect while a job is coming in ends that job and starts the next.
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    SendLpt(p, Text("first"));
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    CHECK(printer.HeldBytes() == 5 && printer.Receiving());
    printer.Clear();
    CHECK(printer.HeldBytes() == 0 && printer.Receiving());

    // IrLAP DISC without an LM disconnect ends the job too.
    SendLpt(p, Text("by DISC"));
    p.Send(p.Command(0x53));
    p.Collect();
    CHECK(!printer.Lap().Connected() && !printer.Receiving());
    // Just after the DISC the guest has only gone quiet; then nothing keeps it running.
    CHECK(printer.NeedsGuest(p.now));
    CHECK(!printer.NeedsGuest(p.now + IrPrinter::guestQuietNs));
    CHECK(printer.HeldBytes() == 7);
}

// Discovery alone, before any link: the guest is mid-print, so it keeps running until it goes quiet.
static void TestDiscoveryNeedsGuest() {
    IrPrinter printer;
    Primary p{printer};
    p.Send(Discovery(p.address, 0x01, 0), 10);
    p.Collect(60 * ms, false);
    CHECK(!printer.Lap().Connected() && printer.NeedsGuest(p.now));
    CHECK(!printer.NeedsGuest(p.now + IrPrinter::guestQuietNs));
}

// ---- Link loss and a load ----

static void TestEndings() {
    IrPrinter printer;
    Primary p{printer};
    const Bytes job = Job(300, 9);

    // The guest stops polling: the watchdog ends the link, and the job is kept as a printer would.
    Connect(p, 0x01);
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    SendLpt(p, job);
    p.Idle(3100 * ms);
    CHECK(!printer.Lap().Connected() && printer.Lap().GetStats().timeouts == 1);
    CHECK(printer.HeldBytes() == job.size());
    printer.Clear();

    // A load or reset mid-job: dropped, nothing held, held output from before kept.
    Connect(p);
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    SendLpt(p, job);
    p.Exchange({uint8_t(0x80 | IrPrinter::lptLsap), p.lptPeer, 0x02, 0x01});
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    SendLpt(p, Text("half a job"));
    printer.Drop(p.now);
    CHECK(!printer.Lap().Connected() && !printer.Receiving());
    CHECK(printer.HeldBytes() == job.size() && printer.HeldJobs() == 1 && printer.GetStats().dropped == 1);

    // Data for an LSAP after the link went: refused, nothing added.
    Connect(p);
    p.Exchange(Cat({IrPrinter::lptLsap, p.lptPeer}, Text("stale")));
    CHECK(!printer.Receiving() && printer.HeldBytes() == job.size());
}

// ---- IrCOMM, 9-wire cooked and 3-wire raw ----

static void TestIrComm() {
    IrPrinter printer;
    Primary p{printer};
    Connect(p);

    // Tiny TP connect with credit, then SDUs with an empty control channel.
    const Bytes job = Job(200, 40);
    LmConnect(p, p.commLsap, IrComm::cookedLsap, {0x01, 0x00});
    CHECK(printer.Comm().GetMode() == IrComm::Mode::Cooked && printer.Receiving());
    for (size_t at = 0; at < job.size(); at += 50)
        p.Exchange(Cat({IrComm::cookedLsap, p.commLsap, 0x01, 0x00}, Bytes(job.begin() + at, job.begin() + at + 50)));
    p.Exchange({uint8_t(0x80 | IrComm::cookedLsap), p.commLsap, 0x02, 0x01});
    CHECK(printer.Comm().GetMode() == IrComm::Mode::Closed);
    CHECK(printer.HeldBytes() == job.size() && printer.HeldJobs() == 1);

    // 3-wire raw: IrLMP data straight in.
    LmConnect(p, 0x09, IrComm::rawLsap, {});
    CHECK(printer.Comm().GetMode() == IrComm::Mode::Raw);
    p.Exchange(Cat({IrComm::rawLsap, 0x09}, Text("raw\r\n")));
    p.Exchange({uint8_t(0x80 | IrComm::rawLsap), 0x09, 0x02, 0x01});
    CHECK(printer.HeldJobs() == 2);

    Capture capture;
    CHECK(printer.Save(Capture::Output, &capture) == RawPrintJob::State::published);
    CHECK(capture.outputs.size() == 1 && capture.outputs[0] == Cat(job, Text("raw\r\n")));
}

// ---- Overflow ----

static void TestOverflow() {
    IrPrinter::Options options;
    options.limit = 100;
    IrPrinter printer{options};
    Primary p{printer};
    Connect(p);

    // A job past the limit is dropped, and the guest gets an LM disconnect.
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    p.pdus.clear();
    SendLpt(p, Job(150, 1));
    bool told = false;
    for (const Bytes& pdu : p.pdus)
        told = told || (pdu.size() >= 3 && pdu[0] == (0x80 | p.lptPeer) && pdu[1] == IrPrinter::lptLsap && pdu[2] == 0x02);
    CHECK(told);
    CHECK(!printer.Receiving() && printer.HeldBytes() == 0 && printer.GetStats().overflows == 1);

    // Jobs that fit alone but not together: the one that doesn't fit goes, the held output stays.
    for (int i = 0; i < 2; ++i) {
        CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
        SendLpt(p, Job(60, uint8_t(i)));
        p.Exchange({uint8_t(0x80 | IrPrinter::lptLsap), p.lptPeer, 0x02, 0x01});
    }
    CHECK(printer.HeldBytes() == 60 && printer.HeldJobs() == 1 && printer.GetStats().overflows == 2);
    printer.Clear();

    // IrCOMM cooked: the Tiny TP LSAP is disconnected too, so the job isn't dropped silently.
    LmConnect(p, p.commLsap, IrComm::cookedLsap, {0x01, 0x00});
    CHECK(printer.Comm().GetMode() == IrComm::Mode::Cooked && printer.Receiving());
    p.pdus.clear();
    const Bytes big = Job(150, 5);
    for (size_t at = 0; at < big.size(); at += 50)
        p.Exchange(Cat({IrComm::cookedLsap, p.commLsap, 0x01, 0x00}, Bytes(big.begin() + at, big.begin() + at + 50)));
    told = false;
    for (const Bytes& pdu : p.pdus)
        told = told || (pdu.size() >= 3 && pdu[0] == (0x80 | p.commLsap) && pdu[1] == IrComm::cookedLsap && pdu[2] == 0x02);
    CHECK(told);
    CHECK(printer.Comm().GetMode() == IrComm::Mode::Closed);
    CHECK(!printer.Receiving() && printer.HeldBytes() == 0 && printer.GetStats().overflows == 3);

    // IrCOMM 3-wire raw the same.
    LmConnect(p, 0x09, IrComm::rawLsap, {});
    CHECK(printer.Comm().GetMode() == IrComm::Mode::Raw && printer.Receiving());
    p.pdus.clear();
    for (size_t at = 0; at < big.size(); at += 50)
        p.Exchange(Cat({IrComm::rawLsap, 0x09}, Bytes(big.begin() + at, big.begin() + at + 50)));
    told = false;
    for (const Bytes& pdu : p.pdus)
        told = told || (pdu.size() >= 3 && pdu[0] == (0x80 | 0x09) && pdu[1] == IrComm::rawLsap && pdu[2] == 0x02);
    CHECK(told);
    CHECK(printer.Comm().GetMode() == IrComm::Mode::Closed);
    CHECK(!printer.Receiving() && printer.HeldBytes() == 0 && printer.GetStats().overflows == 4);

    // The link is still there for the next job.
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    SendLpt(p, Text("after"));
    p.Exchange({uint8_t(0x80 | IrPrinter::lptLsap), p.lptPeer, 0x02, 0x01});
    CHECK(printer.HeldBytes() == 5);
}

// ---- The irprinter command ----

static void TestControl() {
    IrPrinterControl control;
    std::string result;
    CHECK(control.Command(nullptr, "status", "", result) && result == "off off bytes=0 receiving=0 last=none idle=-1");
    result.clear();
    CHECK(!control.Command(nullptr, "save", "/nowhere", result));

    IrPrinter printer;
    Primary p{printer};
    result.clear();
    CHECK(control.Command(&printer, "status", "", result) && result == "attached ready bytes=0 receiving=0 last=none idle=-1");
    Connect(p);
    CHECK(LmConnect(p, p.lptPeer, IrPrinter::lptLsap, {}).empty());
    control.Observe(&printer);
    result.clear();
    control.Command(&printer, "status", "", result);
    CHECK(result.rfind("attached receiving bytes=0 receiving=0 last=none idle=", 0) == 0 && result.find("idle=-1") == std::string::npos);
    const Bytes job = Job(500, 77);
    SendLpt(p, job);
    // A job still coming in isn't held yet, but the status line counts it.
    result.clear();
    control.Command(&printer, "status", "", result);
    CHECK(result.rfind("attached receiving bytes=0 receiving=500 last=none idle=", 0) == 0);
    p.Exchange({uint8_t(0x80 | IrPrinter::lptLsap), p.lptPeer, 0x02, 0x01});

    char folder[] = "/tmp/frond-ir-printer-XXXXXX";
    CHECK(mkdtemp(folder) != nullptr);
    const std::string path = std::string(folder) + "/job.raw";
    result.clear();
    CHECK(control.Command(&printer, "save", path, result));
    CHECK(result.rfind("attached ready bytes=0 receiving=0 last=published idle=", 0) == 0);
    std::ifstream in(path, std::ios::binary);
    CHECK(Bytes(std::istreambuf_iterator<char>(in), {}) == job);
    // The output never overwrites, and with nothing held there is nothing to save.
    result.clear();
    CHECK(!control.Command(&printer, "save", path, result) && result.rfind("Nothing to save.", 0) == 0);
    result.clear();
    CHECK(!control.Command(&printer, "print", "", result));
    unlink(path.c_str());
    rmdir(folder);
}

int main() {
    TestDiscoveryNeedsGuest();
    TestLptJob();
    TestEndings();
    TestIrComm();
    TestOverflow();
    TestControl();
    printf("ir-printer: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
