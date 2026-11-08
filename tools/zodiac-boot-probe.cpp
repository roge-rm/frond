#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include "device_profile5.h"
#include "imx1_boot_probe.h"
#include "imx1_i2c_read_window.h"

static bool peripheralTrace = false;
static bool frameOutputFailed = false;
static void captureFrame(void* context, uint32_t width, uint32_t height, const uint32_t* pixels) {
    std::ofstream output(static_cast<const char*>(context), std::ios::binary);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (uint32_t i = 0; i < width * height; ++i)
        for (unsigned lane = 0; lane < 3; ++lane) output.put(char(pixels[i] >> (lane * 8)));
    frameOutputFailed = !output;
}
static void trace(void*, const Imx1ProbeTrace& t) {
    if (peripheralTrace) return;
    if (t.instructionSize == 2) {
        std::printf("step=%u pc=%08x instruction=%04x cpsr=%08x operand=%08x width=2\n",
                    t.step, t.pc, t.instruction, t.cpsr, t.operand);
        return;
    }
    std::printf("step=%u pc=%08x instruction=%08x cpsr=%08x operand=%08x width=4\n",
                t.step, t.pc, t.instruction, t.cpsr, t.operand);
}
static void bus(void* context, const Imx1ProbeAccess& t) {
    if (peripheralTrace && t.status == Imx1BootstrapAccess::ok && t.address >= 0x08000000 &&
        !(t.address >= 0x15000000 && t.address < 0x17000000)) return;
    const char* name = Imx1Bootstrap::registerName(t.address);
    if (!name && t.address >= 0x08000000 && t.address < 0x10000000) name = "SDRAM";
    std::printf("bus step=%u pc=%08x operation=%s address=%08x size=%u value=%08x register=%s status=%s%s",
                t.step, t.pc, t.write ? "write" : "read", t.address, t.size, t.value,
                name ? name : "unknown", imx1BootstrapAccessName(t.status),
                (t.diagnosticCs5Configuration || t.diagnosticCs5Reset) &&
                    static_cast<Imx1ProbeOptions*>(context)->graphicsProfile != Imx1GraphicsProfile::strict ? " source=virtual5744-compatible-8mib" :
                t.diagnosticCs5Configuration ? " source=diagnostic-cs5-configuration" :
                t.diagnosticCs5Reset ? " source=diagnostic-cs5-reset" :
                t.diagnosticCs4 ? " source=diagnostic-cs4-capture" : "");
    if (t.knownMask != 0xffffffffu) std::printf(" known-mask=%08x single-pin-query", t.knownMask);
    if (t.cs5WaitClocks) std::printf(" cs5-hclk=%u",t.cs5WaitClocks);
    std::putchar('\n');
}

static void cs5Reset(void* context, const Imx1ProbeCs5ResetEvent& event) {
    std::printf("cs5-reset step=%u pc=%08x operation=%s address=%08x size=%u value=%02x phase=%u status=%s source=%s\n",
                event.step, event.pc, event.write ? "write" : "read", event.address, event.size,
                event.value, event.phase, imx1BootstrapAccessName(event.status),
                static_cast<Imx1ProbeOptions*>(context)->graphicsProfile != Imx1GraphicsProfile::strict ?
                    "virtual5744-compatible-8mib" : "diagnostic-reset-echo");
}

static void cs5Configuration(void* context, const Imx1ProbeCs5ConfigurationEvent& event) {
    std::printf("cs5-configuration step=%u pc=%08x operation=%s address=%08x size=%u value=%08x defaults-known=%u status=%s source=%s",
                event.step, event.pc, event.write ? "write" : "read", event.address, event.size,
                event.value, event.defaultsKnown, imx1BootstrapAccessName(event.status),
                static_cast<Imx1ProbeOptions*>(context)->graphicsProfile != Imx1GraphicsProfile::strict ?
                    "virtual5744-compatible-8mib" : "configured-virtual-aperture");
    if (event.knownMask != 0xffffffffu) std::printf(" known-mask=%08x single-pin-query", event.knownMask);
    std::putchar('\n');
}

static void cs4(void*, const Imx1ProbeCs4Event& event) {
    char index[12], bank[12];
    if (event.index < 0) std::strcpy(index, "unknown");
    else std::snprintf(index, sizeof(index), "%02x", event.index);
    if (event.bank < 0) std::strcpy(bank, "unknown");
    else std::snprintf(bank, sizeof(bank), "%d", event.bank);
    const char* operation = event.address == 0x15000000 ?
        (event.write ? "index-write" : "raw-port0-input") : event.address == 0x15000001 ?
        (event.write ? "data-command" : "data-read") : "unmapped";
    const bool indexedSample = !event.write && event.address == 0x15000001 && event.status == Imx1BootstrapAccess::ok;
    if (indexedSample) operation = "raw-indexed-input";
    std::printf("cs4-capture step=%u pc=%08x operation=%s address=%08x size=%u value=%02x index=%s bank=%s status=%s source=%s\n",
                event.step, event.pc, operation, event.address, event.size, event.value, index, bank,
                imx1BootstrapAccessName(event.status), indexedSample ? "diagnostic-indexed-input" : "diagnostic-recorder");
}

static void i2c(void*, const Imx1ProbeI2cEvent& event) {
    const auto& t = event.transfer;
    std::printf("i2c step=%u pc=%08x issue-step=%u issue-pc=%08x phase=%s address=%02x byte=%02x direction=%s master-ack=%u response=%s control=%02x status=%02x hclk-divider=%u irq-source=39\n",
                event.step, event.pc, event.issueStep, event.issuePc, imx1I2cPhaseName(t.phase),
                t.address, t.byte, t.read ? "read" : "write", t.acknowledge, imx1I2cResponseName(event.response),
                event.control, event.status, event.divider);
}
static void interrupt(void*, const Imx1ProbeInterrupt& event) {
    std::printf("interrupt step=%u interrupted-pc=%08x cpsr=%08x vector=%08x link=%08x type=%s normal-pending=%016llx fast-pending=%016llx\n",
                event.step, event.interruptedPc, event.cpsr, event.vector, event.link, event.fiq ? "fiq" : "irq",
                (unsigned long long)event.pending.normal, (unsigned long long)event.pending.fast);
}
static bool parseCs4IndexedInput(const char* text, Imx1Cs4IndexedInputs& inputs) {
    uint32_t fields[3]{};
    for (unsigned i = 0; i < 3; ++i) {
        if (*text < '0' || *text > '9') return false;
        errno = 0;
        char* end = nullptr;
        const auto value = std::strtoul(text, &end, 0);
        if (errno || value > (i ? 255u : 1u) || *end != (i == 2 ? '\0' : '/')) return false;
        fields[i] = uint32_t(value);
        text = end + (i != 2);
    }
    return inputs.set(fields[0], fields[1], fields[2]);
}
static bool parseRtcTime(const char* text, Imx1RtcTime& time) {
    uint32_t fields[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        if (*text < '0' || *text > '9') return false;
        while (*text >= '0' && *text <= '9') {
            if (fields[i] > 511) return false;
            fields[i] = fields[i] * 10 + unsigned(*text++ - '0');
        }
        if (i < 3) { if (*text++ != ':') return false; }
        else if (*text) return false;
    }
    time = {fields[0], fields[1], fields[2], fields[3]};
    return Imx1Rtc::validTime(time);
}
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s ROM [physical|reset|linked] [instruction-budget] [exact|fast] [--bootstrap-peripherals] [--trace=peripherals] [--arm920-icache-index-compatibility] [--lcdc-contrast-only] [--frame-output=PATH] [--assume-mpll-settled] [--assume-spll-settled] [--assume-i2c-single-master] [--i2c-eeprom-image=PATH] [--asp-px1-input=high] [--asp-u-sample=WORD] [--rtc-initial-time=DAY:HH:MM:SS] [--cs4-port0-input=BYTE] [--cs4-indexed-input=BANK/INDEX/BYTE] [--cs5-reset-status-input=BYTE] [--graphics-profile=virtual5744-compatible-8mib] [--graphics-raw-axes=FIRST/SECOND] [--graphics-irq-connection=virtual-a13] [--gpio-a-input-masked=VALUE/MASK] [--gpio-a-input=VALUE] [--gpio-b-input=VALUE] [--gpio-d-input=VALUE] [--ram-mib=32|128]\n", argv[0]);
        return 1;
    }
    const char* engine = argc >= 5 ? argv[4] : "exact";
    if (std::strcmp(engine, "exact") && std::strcmp(engine, "fast")) {
        std::fprintf(stderr, "Engine must be exact or fast\n"); return 1;
    }
    const char* i2cImagePath = nullptr;
    Imx1ProbeOptions options{};
    options.interruptObserver = interrupt;
    options.busObserver = bus;
    options.i2cObserver = i2c;
    options.cs4Observer = cs4;
    options.cs5ResetObserver = cs5Reset;
    options.cs5ConfigurationObserver = cs5Configuration;
    options.busContext = options.cs5ResetContext = options.cs5ConfigurationContext = &options;
    for (int i = 5; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bootstrap-peripherals")) options.bootstrapPeripherals = true;
        else if (!std::strcmp(argv[i], "--lcdc-contrast-only")) {
            if (options.lcdcContrastOnly) return 1;
            options.lcdcContrastOnly = true;
        }
        else if (!std::strcmp(argv[i], "--arm920-icache-index-compatibility")) {
            if (options.arm920IcacheIndexCompatibility) return 1;
            options.arm920IcacheIndexCompatibility = true;
        }
        else if (!std::strcmp(argv[i], "--trace=peripherals")) peripheralTrace = true;
        else if (!std::strncmp(argv[i], "--frame-output=", 15) && argv[i][15]) {
            if (options.frameObserver) return 1;
            options.frameObserver = captureFrame;
            options.frameContext = argv[i] + 15;
        }
        else if (!std::strcmp(argv[i], "--assume-i2c-single-master")) options.assumeI2cSingleMaster = true;
        else if (!std::strncmp(argv[i], "--graphics-profile=", 19)) {
            if (options.graphicsProfile != Imx1GraphicsProfile::strict ||
                std::strcmp(argv[i] + 19, "virtual5744-compatible-8mib")) {
                std::fprintf(stderr, "Supply --graphics-profile=virtual5744-compatible-8mib once\n"); return 1;
            }
            options.graphicsProfile = Imx1GraphicsProfile::virtual5744Compatible8MiB;
        }
        else if (!std::strncmp(argv[i], "--graphics-raw-axes=", 20)) {
            const char* text = argv[i] + 20;
            char* end = nullptr;
            errno = 0;
            const auto first = std::strtoul(text, &end, 0);
            if (options.graphicsRawAxesKnown || errno || *text < '0' || *text > '9' || *end != '/' || first > 1023) return 1;
            text = end + 1; errno = 0;
            const auto second = std::strtoul(text, &end, 0);
            if (errno || *text < '0' || *text > '9' || *end || second > 1023) return 1;
            options.graphicsRawAxesKnown = true;
            options.graphicsRawFirst = first; options.graphicsRawSecond = second;
        }
        else if (!std::strncmp(argv[i], "--graphics-irq-connection=", 26)) {
            if (options.graphicsIrqA13 || std::strcmp(argv[i] + 26, "virtual-a13")) {
                std::fprintf(stderr, "Supply --graphics-irq-connection=virtual-a13 once\n"); return 1;
            }
            options.graphicsIrqA13 = true;
        }
        else if (!std::strncmp(argv[i], "--gpio-a-input-masked=", 22)) {
            const char* text = argv[i] + 22;
            char* end = nullptr;
            errno = 0;
            const auto value = std::strtoul(text, &end, 0);
            if (options.gpioAInputMaskedKnown || errno || *text < '0' || *text > '9' || *end != '/' || value > UINT32_MAX) {
                std::fprintf(stderr, "Supply one --gpio-a-input-masked=VALUE/MASK\n"); return 1;
            }
            text = end + 1;
            errno = 0;
            const auto mask = std::strtoul(text, &end, 0);
            if (errno || *text < '0' || *text > '9' || *end || !mask || mask > UINT32_MAX || (value & ~mask)) {
                std::fprintf(stderr, "GPIO-A masked value must fit its nonzero 32-bit mask\n"); return 1;
            }
            options.gpioAInputMaskedKnown = true;
            options.gpioAInput = value;
            options.gpioAInputMask = mask;
        }
        else if (!std::strncmp(argv[i], "--rtc-initial-time=", 19)) {
            if (options.rtcInitialTimeKnown || !parseRtcTime(argv[i] + 19, options.rtcInitialTime)) {
                std::fprintf(stderr, "Supply one --rtc-initial-time=DAY:HH:MM:SS (0..511:0..23:0..59:0..59)\n"); return 1;
            }
            options.rtcInitialTimeKnown = true;
        }
        else if (!std::strncmp(argv[i], "--cs4-port0-input=", 18)) {
            char* end = nullptr;
            errno = 0;
            const char* text = argv[i] + 18;
            const auto value = std::strtoul(text, &end, 0);
            if (options.cs4Port0InputKnown || errno || *text < '0' || *text > '9' || *end || value > 0xff) {
                std::fprintf(stderr, "Supply one 8-bit --cs4-port0-input value\n"); return 1;
            }
            options.cs4Port0InputKnown = true;
            options.cs4Port0Input = value;
        }
        else if (!std::strncmp(argv[i], "--cs4-indexed-input=", 20)) {
            if (!parseCs4IndexedInput(argv[i] + 20, options.cs4IndexedInputs)) {
                std::fprintf(stderr, "Supply unique --cs4-indexed-input=BANK/INDEX/BYTE (bank 0..1, bytes 0..255)\n"); return 1;
            }
        }
        else if (!std::strncmp(argv[i], "--cs5-reset-status-input=", 25)) {
            char* end = nullptr;
            errno = 0;
            const char* text = argv[i] + 25;
            const auto value = std::strtoul(text, &end, 0);
            if (options.cs5ResetStatusInputKnown || errno || *text < '0' || *text > '9' || *end || value > 0xff) {
                std::fprintf(stderr, "Supply one 8-bit --cs5-reset-status-input value\n"); return 1;
            }
            options.cs5ResetStatusInputKnown = true;
            options.cs5ResetStatusInput = value;
        }
        else if (!std::strncmp(argv[i], "--asp-px1-input=", 16)) {
            if (options.aspPx1HighFromReset || std::strcmp(argv[i] + 16, "high")) {
                std::fprintf(stderr, "Supply --asp-px1-input=high once, held from reset\n"); return 1;
            }
            options.aspPx1HighFromReset = true;
        }
        else if (!std::strncmp(argv[i], "--asp-u-sample=", 15)) {
            char* end = nullptr;
            errno = 0;
            const char* text = argv[i] + 15;
            const auto value = std::strtoul(text, &end, 0);
            if (options.aspUSampleKnown || errno || *text < '0' || *text > '9' || *end || value > 0xffff) {
                std::fprintf(stderr, "Supply one 16-bit --asp-u-sample value\n"); return 1;
            }
            options.aspUSampleKnown = true;
            options.aspUSample = value;
        }
        else if (!std::strncmp(argv[i], "--i2c-eeprom-image=", 19)) {
            if (i2cImagePath || !argv[i][19]) {
                std::fprintf(stderr, "Supply one nonempty --i2c-eeprom-image path\n"); return 1;
            }
            i2cImagePath = argv[i] + 19;
        }
        else if (!std::strcmp(argv[i], "--assume-spll-settled")) options.assumeSpllSettled = true;
        else if (!std::strcmp(argv[i], "--assume-mpll-settled")) options.assumeMpllSettled = true;
        else if (!std::strncmp(argv[i], "--gpio-a-input=", 15) ||
                 !std::strncmp(argv[i], "--gpio-b-input=", 15) ||
                 !std::strncmp(argv[i], "--gpio-d-input=", 15)) {
            const char port = argv[i][7];
            char* end = nullptr;
            errno = 0;
            const auto value = std::strtoul(argv[i] + 15, &end, 0);
            if (errno || argv[i][15] < '0' || argv[i][15] > '9' || *end || value > UINT32_MAX) {
                std::fprintf(stderr, "GPIO-%c input must be a 32-bit value\n", port - 'a' + 'A'); return 1;
            }
            if (port == 'a') { options.gpioAInputKnown = true; options.gpioAInput = value; }
            else if (port == 'b') { options.gpioBInputKnown = true; options.gpioBInput = value; }
            else { options.gpioDInputKnown = true; options.gpioDInput = value; }
        } else if (!std::strcmp(argv[i], "--ram-mib=32")) options.ramMiB = 32;
        else if (!std::strcmp(argv[i], "--ram-mib=128")) options.ramMiB = 128;
        else { std::fprintf(stderr, "Unknown diagnostic option: %s\n", argv[i]); return 1; }
    }
    if (options.graphicsRawAxesKnown && (!options.bootstrapPeripherals ||
        options.graphicsProfile != Imx1GraphicsProfile::virtual5744Compatible8MiB)) {
        std::fprintf(stderr, "Raw graphics axes require the virtual graphics profile and bootstrap peripherals\n"); return 1;
    }
    if ((options.gpioAInputKnown && options.gpioAInputMaskedKnown) ||
        (options.graphicsIrqA13 && (options.graphicsProfile != Imx1GraphicsProfile::virtual5744Compatible8MiB ||
          !options.bootstrapPeripherals || options.gpioAInputKnown || (options.gpioAInputMask & 0x2000)))) {
        std::fprintf(stderr, "Virtual GPU IRQ requires the virtual graphics profile and bootstrap; GPIO-A13 is reserved, omit full-port GPIO-A input and exclude bit13 from its masked input\n"); return 1;
    }
    if (options.lcdcContrastOnly) {
        if (!options.bootstrapPeripherals) return 1;
        std::printf("LCDC contrast-only virtual policy: LCD-clock PWM, no internal panel or DMA.\n");
    }
    if (options.arm920IcacheIndexCompatibility)
        std::printf("ARM920T compatibility: undocumented c7,c5,2 flushes the whole I-cache; no silicon claim.\n");
    if (options.graphicsProfile != Imx1GraphicsProfile::strict &&
        (!options.bootstrapPeripherals || options.cs5ResetStatusInputKnown)) {
        std::fprintf(stderr, "Graphics profile requires --bootstrap-peripherals and owns CS5 reset state; omit --cs5-reset-status-input\n"); return 1;
    }
    if ((options.aspUSampleKnown || options.cs5ResetStatusInputKnown || options.rtcInitialTimeKnown || options.cs4Port0InputKnown || options.aspPx1HighFromReset || options.gpioAInputKnown || options.gpioAInputMaskedKnown || options.gpioBInputKnown || options.gpioDInputKnown || options.ramMiB) && !options.bootstrapPeripherals) {
        std::fprintf(stderr, "External-input scenarios require --bootstrap-peripherals\n"); return 1;
    }
    if (!options.cs4IndexedInputs.empty() && (!options.bootstrapPeripherals || !options.cs4Port0InputKnown)) {
        std::fprintf(stderr, "CS4 indexed inputs require --bootstrap-peripherals and --cs4-port0-input\n"); return 1;
    }
    if (options.assumeMpllSettled && !options.bootstrapPeripherals) {
        std::fprintf(stderr, "MPLL scenario requires --bootstrap-peripherals\n"); return 1;
    }
    if (options.assumeSpllSettled && !options.bootstrapPeripherals) {
        std::fprintf(stderr, "SPLL scenario requires --bootstrap-peripherals\n"); return 1;
    }
    if (options.assumeI2cSingleMaster && !options.bootstrapPeripherals) {
        std::fprintf(stderr, "I2C scenario requires --bootstrap-peripherals\n"); return 1;
    }
    std::unique_ptr<Imx1I2cReadWindow> i2cWindow;
    if (i2cImagePath) {
        if (!options.bootstrapPeripherals || !options.assumeI2cSingleMaster) {
            std::fprintf(stderr, "I2C image requires --bootstrap-peripherals and --assume-i2c-single-master\n"); return 1;
        }
        std::ifstream image(i2cImagePath, std::ios::binary);
        Imx1I2cReadWindow::Image bytes{};
        if (!image.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
            image.peek() != std::char_traits<char>::eof() || image.bad()) {
            std::fprintf(stderr, "Expected an exact 256-byte I2C read-window image\n"); return 1;
        }
        i2cWindow = std::make_unique<Imx1I2cReadWindow>(bytes);
        options.i2cBus = i2cWindow.get();
    }
    unsetenv("FROND_JIT");
    const char* name = argc >= 3 ? argv[2] : "physical";
    Imx1ProbeEntry entry;
    if (!std::strcmp(name, "physical")) entry = Imx1ProbeEntry::physicalFlash;
    else if (!std::strcmp(name, "reset")) entry = Imx1ProbeEntry::resetAlias;
    else if (!std::strcmp(name, "linked")) entry = Imx1ProbeEntry::linkedAlias;
    else { std::fprintf(stderr, "Unknown entry mapping: %s\n", name); return 1; }
    unsigned long budget = 100000;
    if (argc >= 4) {
        char* end = nullptr;
        errno = 0;
        budget = std::strtoul(argv[3], &end, 10);
        if (errno || !*argv[3] || *end || !budget || budget > imx1ProbeMaximumSteps) {
            std::fprintf(stderr, "Instruction budget must be 1..%u\n", imx1ProbeMaximumSteps);
            return 1;
        }
    }
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != 0x7d0000) {
        std::fprintf(stderr, "Expected an exact 0x7d0000-byte Zodiac ROM\n");
        return 1;
    }
    input.seekg(0);
    std::vector<uint8_t> rom(0x7d0000);
    if (!input.read(reinterpret_cast<char*>(rom.data()), rom.size())) {
        std::fprintf(stderr, "Cannot read ROM\n"); return 1;
    }
    const auto identity = deviceProfileInspectNor5(rom.size(), [&](uint64_t offset, void* data, size_t size) {
        if (offset > rom.size() || size > rom.size() - offset) return false;
        std::memcpy(data, rom.data() + offset, size); return true;
    });
    if (!identity.profile || identity.profile->deviceType != deviceTypeZodiac2 || !identity.validLayout) {
        std::fprintf(stderr, "ROM does not match the staged Zodiac family profile\n"); return 1;
    }
    std::printf("Experimental i.MX1 SmallROM probe; entry=%s budget=%lu engine=%s\n", name, budget, engine);
    std::puts("Physical flash=10000000; VINITHI low assumed.");
    if (!options.bootstrapPeripherals) std::puts("No RAM or MMIO model.");
    else {
        std::puts("Documented bootstrap register subset enabled; unsupported operations stop.");
        std::printf("MPLL settled assumption=%s; nominal FCLK only after completed restart; no lock claim.\n",
                    options.assumeMpllSettled ? "enabled" : "disabled");
        std::printf("SPLL settled assumption=%s; initial nominal output/LF=1; restart holds output high for 2 CLK32 then activates target/LF=1 with phase zero; bus waits for that event without retiring instructions.\n",
                    options.assumeSpllSettled ? "enabled" : "disabled");
        std::printf("I2C single-master initially-idle scenario=%s; no stretching; unknown slave responses stop.\n",
                    options.assumeI2cSingleMaster ? "enabled" : "disabled");
        if (options.rtcInitialTimeKnown) {
            const auto t = options.rtcInitialTime;
            std::printf("Diagnostic RTC initial time=%u:%02u:%02u:%02u; opaque chip day, CLK32 subsecond phase zero at entry; elapsed reference clocks only, no civil epoch or host time.\n", t.day, t.hour, t.minute, t.second);
        } else std::puts("Retained RTC day/time and subsecond phase are unknown.");
        if (options.graphicsProfile != Imx1GraphicsProfile::strict)
            std::printf("VIRTUAL GRAPHICS: virtual5744-compatible-8mib; identity=%08x revision=%08x capacity=%u are model choices; external bank plus %u-byte internal bank; bounded direct backing, configured windows and paired lookup storage; cold VRAM zero, soft reset retains bytes; wrap and other apertures unbacked; GPIO direction 1 drives ideal output pads under selector ffff, external inputs unknown; reset clears GPIO state; reset status=00, REG_BASE=01, CPU_DEFAULTS=0e transaction; unsupported controller operations stop.\n",
                        Imx1Cs5Configuration::virtualIdentity, Imx1Cs5Configuration::virtualRevision,
                        Imx1Cs5Configuration::virtualExternalBytes, Imx1Cs5Configuration::virtualInternalBytes);
        if (options.graphicsProfile != Imx1GraphicsProfile::strict)
            std::puts("VIRTUAL DISPLAY: before programming, pixel source=1 pixel per 16 charged CPU cycles; programmed PCLK follows graphics clocks; 32-cycle initial or 32-tick programmed settling, running latch at next frame boundary; framebuffer RGB565 to host ABGR8888; startup option nibble=0 on pins9..12 after display enable, per-pin observations only; no physical clock, panel timing or board strap claim.");
        if (options.graphicsProfile != Imx1GraphicsProfile::strict)
            std::printf("VIRTUAL GRAPHICS CLOCKS: reference=16000000Hz, time=200000000Hz from Cgm/CLK32 guest time; counter=source/4 with 4*(lock+1) reference-period gate; register-driven nominal VCO curve, no physical lock or oscillator claim.\n");
        if (options.graphicsProfile != Imx1GraphicsProfile::strict)
            std::puts("VIRTUAL CS5 BUS: fixed WSC62=63 HCLK stalls; DTACK=fresh falling edge, WAIT=ready high; one live SCLK service cycle and minimum3 HCLK; timeout1022 HCLK, response on final edge wins; HCLK from Cgm with nominal CLK32=32768Hz; GPIO SSR sample independent; no physical timing claim.");
        if (options.cs5ResetStatusInputKnown)
            std::printf("DIAGNOSTIC CS5 RESET: opaque status input=%02x is caller-supplied and unqualified; bit3 echoes reset-helper writes immediately; other bits, chip reset effects and timing are unmodelled. Configured virtual REG_BASE=01 selects +10000; exact CPU_DEFAULTS=0e is recorded; full identity/revision words remain unknown.\n", options.cs5ResetStatusInput);
        if (options.cs4Port0InputKnown)
            std::printf("DIAGNOSTIC CS4 RECORDER: raw port0 input=%02x is caller-supplied and unqualified; writes are captured; physical read side effects, audio, FIFO and timing are unmodelled; %sdata reads stop.\n", options.cs4Port0Input, options.cs4IndexedInputs.empty() ? "" : "unsupplied ");
        for (int bank = 0; bank < 2; ++bank) for (int index = 0; index < 256; ++index) {
            const int sample = options.cs4IndexedInputs.sample(bank, index);
            if (sample >= 0) std::printf("DIAGNOSTIC CS4 INDEXED INPUT: bank=%d index=%02x byte=%02x; fixed caller sample, no register/read-side-effect model; writes do not update it.\n", bank, index, sample);
        }
        if (options.aspPx1HighFromReset)
            std::puts("Diagnostic ASP PX1 input held high from reset; no transitions; physical voltage and ADC samples unqualified.");
        else std::puts("External ASP PX1 level and history are unknown.");
        if (options.aspUSampleKnown)
            std::printf("Diagnostic ASP manual U post-filter sample=%04x; U_SEL=0, BIT_SELECT=2; fixed virtual stimulus, no physical voltage or calibration claim.\n", options.aspUSample);
        else std::puts("External ASP U conversion input is unknown.");
        if (i2cWindow) std::puts("Explicit 256-byte I2C 0x50 read window attached; caller-supplied bytes; physical identity and contents unqualified.");
        if (options.gpioAInputKnown)
            std::printf("Hypothetical static from-reset full-port GPIO-A input=%08x\n", options.gpioAInput);
        else if (options.gpioAInputMaskedKnown)
            std::printf("Hypothetical static from-reset masked GPIO-A input=%08x mask=%08x; other lanes have separate owners or remain unknown.\n", options.gpioAInput, options.gpioAInputMask);
        else std::puts("External GPIO-A levels are unknown.");
        if (options.graphicsIrqA13)
            std::puts("VIRTUAL GPU IRQ: virtual-a13; GPU interrupt owns i.MX1 GPIO-A13 from reset, asserted low, released resolved high by an ideal virtual pull-up; pending and enables determine its level; inferred topology, no physical wiring claim.");
        if (options.gpioBInputKnown)
            std::printf("Hypothetical static from-reset full-port GPIO-B input=%08x\n", options.gpioBInput);
        else std::puts("External GPIO-B levels are unknown.");
        if (options.gpioDInputKnown)
            std::printf("Hypothetical static from-reset full-port GPIO-D input=%08x\n", options.gpioDInput);
        else std::puts("External GPIO-D levels are unknown.");
        std::puts("Timer 2 toggle mode requires D31 GPIO input or DR-selected output; TMR2OUT waveform and board effects remain unmodelled.");
        std::puts("GPIO pad model=ideal digital DR-selected GPIO output without contention; SSR uses output feedback, other supplied levels remain external hypotheses; no synchronizer timing claim.");
        std::printf("Diagnostic RAM capacity=%u MiB, initial contents zero; bus timing unqualified.\n", options.ramMiB);
    }
    if (entry != Imx1ProbeEntry::physicalFlash) std::puts("Selected alias is an unverified mapping experiment.");
    const auto result = imx1BootProbe(rom.data(), rom.size(), entry, budget, trace, nullptr,
                                     !std::strcmp(engine, "fast"), &options);
    std::printf("stop=%s step=%u pc=%08x instruction=%0*x address=%08x size=%u value=%08x operand=%08x width=%u\n",
                imx1ProbeStopName(result.stop), result.trace.step, result.trace.pc,
                int(result.trace.instructionSize * 2), result.trace.instruction, result.address, result.size, result.value,
                result.trace.operand, result.trace.instructionSize);
    std::printf("bus-accesses=%llu peripheral-status=%s\n", (unsigned long long)result.busAccesses,
                imx1BootstrapAccessName(result.peripheralStatus));
    if (options.graphicsProfile != Imx1GraphicsProfile::strict)
        std::printf("cs5-hclk-total=%llu\n",(unsigned long long)result.cs5WaitClocks);
    if (result.possibleWatchdogTimeoutBoundary)
        std::printf("possible-watchdog-timeout-boundary; elapsed reference interval exhausted, reset/IRQ state unknown, no guest MMIO or CPU delivery\n");
    if (result.elapsedInterruptBoundary) {
        const auto pending = result.interruptPending;
        std::printf("elapsed-interrupt-boundary normal-pending=%016llx fast-pending=%016llx unknown-normal=%016llx unknown-fast=%016llx; CPU delivery guarded, no guest access\n",
                    (unsigned long long)pending.normal, (unsigned long long)pending.fast,
                    (unsigned long long)pending.unknownNormal, (unsigned long long)pending.unknownFast);
    }
    if (options.graphicsProfile != Imx1GraphicsProfile::strict)
        std::printf("graphics-frame lcd=%u width=%u height=%u fnv1a=%08x format=ABGR8888\n",
                    unsigned(result.graphicsLcdEnabled), result.graphicsFrameWidth, result.graphicsFrameHeight, result.graphicsFrameHash);
    std::printf("irq-entries=%llu fiq-entries=%llu\n", (unsigned long long)result.irqEntries,
                (unsigned long long)result.fiqEntries);
    if (options.bootstrapPeripherals)
        std::printf("pcm-transport dma-active=%u remaining=%u source=%08x pending-source=%08x fifo-words=%u transmitted-words=%llu disr=%08x scsr=%04x stcr=%04x stccr=%04x\n",
                    unsigned(result.pcm.dmaActive), result.pcm.remaining, result.pcm.source,
                    result.pcm.pendingSource, result.pcm.fifoWords,
                    (unsigned long long)result.pcm.transmittedWords, result.pcm.status,
                    result.pcm.control, result.pcm.transmitControl, result.pcm.clockControl);
    std::printf("jit-blocks=%llu jit-instructions=%llu final-cpsr=%08x\n",
                (unsigned long long)result.jitBlocks, (unsigned long long)result.jitInstructions, result.cpsr);
    for (unsigned i = 0; i < 16; ++i)
        std::printf("r%u=%08x%c", i, result.registers[i], i % 4 == 3 ? '\n' : ' ');
    std::printf("cpsr=%08x spsr=%08x\n", result.cpsr, result.spsr);
    if (frameOutputFailed) { std::fprintf(stderr,"Could not write frame output\n"); return 1; }
    return 2;  // A diagnostic stop never establishes a successful boot.
}
