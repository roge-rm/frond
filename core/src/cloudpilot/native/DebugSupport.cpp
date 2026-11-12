#include "DebugSupport.h"

#include <cstring>
#include <iomanip>
#include <optional>

#include "Debugger.h"
#include "Defer.h"
#include "ElfParser.h"
#include "EmBankSRAM.h"
#include "EmHAL.h"
#include "EmMemory.h"
#include "ROMStubs.h"

namespace {
    emuptr locateCodeResource(const char* name, uint32& size) {
        LocalID lidDB = DmFindDatabase(0, name);
        if (lidDB == 0) {
            cout << "unable to find DB " << name << endl << flush;
            return 0xffffffff;
        }

        uint16 attributes;
        if (DmDatabaseInfo(0, lidDB, nullptr, &attributes, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr) != errNone) {
            cout << "failed to get DB info" << endl << flush;
            return 0xffffffff;
        }

        if ((attributes & dmHdrAttrResDB) == 0) {
            cout << "not a resource DB" << endl << flush;
            return 0xffffffff;
        }

        emuptr dbRef = DmOpenDatabase(0, lidDB, dmModeReadOnly);
        if (dbRef == 0) {
            cout << "unable to open DB" << endl << flush;
            return 0xffffffff;
        }

        Defer deferCloseDB([=]() { DmCloseDatabase(dbRef); });

        emuptr rscHandle = DmGet1Resource('code', 1);
        if (rscHandle == 0) {
            cout << "unable to locate code resource" << endl << flush;
            return 0xffffffff;
        }

        Defer deferReleaseResource([=]() { DmReleaseResource(rscHandle); });

        emuptr rscPtr = MemHandleLock(rscHandle);
        if (rscHandle == 0) {
            cout << "unable to lock code resource" << endl << flush;
            return 0xffffffff;
        }

        size = MemPtrSize(rscPtr);

        MemHandleUnlock(rscHandle);

        return rscPtr;
    }
}  // namespace

namespace {
    // The .text section of an m68k COFF executable, as prc-tools' m68k-palmos-gcc links: file
    // header (20 bytes, big-endian, magic 0x0150), optional header, then 40-byte section headers.
    optional<ElfParser::Section> coffText(const uint8* data, size_t size) {
        auto get16 = [&](size_t at) { return uint32(data[at] << 8 | data[at + 1]); };
        auto get32 = [&](size_t at) { return get16(at) << 16 | get16(at + 2); };
        if (size < 20 || get16(0) != 0x0150) return nullopt;
        const size_t sections = get16(2), first = 20 + get16(16);
        for (size_t i = 0; i < sections && first + 40 * (i + 1) <= size; ++i) {
            const uint8* header = data + first + 40 * i;
            if (memcmp(header, ".text\0", 6) != 0) continue;
            ElfParser::Section section{};
            section.name = ".text";
            section.virtualAddress = get32(first + 40 * i + 12);
            section.size = get32(first + 40 * i + 16);
            section.offset = get32(first + 40 * i + 20);
            if (section.offset > size || section.size > size - section.offset) return nullopt;
            return section;
        }
        return nullopt;
    }
}  // namespace

void debug_support::SetApp(const uint8* elfData, size_t elfSize, const char* dbName,
                           GdbStub& gdbStub, Debugger& debugger) {
    optional<ElfParser::Section> sectionText = coffText(elfData, elfSize);

    if (!sectionText) {
        ElfParser parser;

        try {
            parser.Parse(elfData, elfSize);
        } catch (const ElfParser::EInvalidElf& e) {
            cout << e.GetReason() << endl << flush;
            return;
        }

        if (parser.GetMachine() != 0x04) {
            cout << "bad architecture" << endl << flush;
            return;
        }

        sectionText = parser.GetSection(".text");
    }

    if (!sectionText.has_value() || sectionText->size == 0) {
        cout << ".text not found in binary" << endl << flush;
        return;
    }

    emuptr searchBase = gMemoryStart;
    uint32 searchSize = EmMemory::GetRegionSize(MemoryRegion::ram);

    if (dbName) {
        searchBase = locateCodeResource(dbName, searchSize);
        if (searchBase == 0xffffffff) return;
    }

    const emuptr textBase =
        FindRegion(elfData + sectionText->offset, sectionText->size, searchBase, searchSize);

    if (textBase == 0xffffffff) {
        cout << "unable to locate application in " << (dbName ? "database" : "memory") << endl
             << flush;
        return;
    }

    const int64 relocation = static_cast<int64>(textBase) - sectionText->virtualAddress;
    cout << "found .text relocated by " << relocation << " bytes" << endl;
    cout << "set break mode to app-only" << endl << flush;

    gdbStub.SetRelocationOffset(relocation);
    debugger.SetBreakMode(Debugger::BreakMode::appOnly);
    debugger.SetAppRegion(textBase, sectionText->size);
}

emuptr debug_support::FindRegion(const uint8* region, size_t regionSize, emuptr start,
                                 size_t size) {
    if (regionSize > size) return 0xffffffff;

    // <=: the region may fill the whole range, as an app's code resource does.
    for (emuptr address = start; address <= start + size - regionSize; address++) {
        for (emuptr i = 0; i < regionSize; i++)
            if (region[i] != EmMemGet8(i + address)) goto nextAddress;

        return address;

    nextAddress:;
    }

    return 0xffffffff;
}

void debug_support::Locate(const uint8* data, size_t size) {
    emuptr address = gMemoryStart;
    const size_t ramSize = Memory::GetRegionSize(MemoryRegion::ram);
    bool found = false;

    do {
        address = FindRegion(data, size, address, ramSize + gMemoryStart - address);

        if (address < 0xffffffff) {
            cout << "located file content in RAM at 0x" << hex << setfill('0') << setw(8) << address
                 << dec << endl
                 << flush;

            found = true;
        } else {
            break;
        }

        address += size;
    } while (address >= gMemoryStart && address < gMemoryStart + ramSize);

    const emuptr romStart = EmHAL::GetROMBaseAddress();
    const size_t romSize = EmHAL::GetROMSize();
    address = romStart;

    do {
        address = FindRegion(data, size, address, romSize + romStart - address);

        if (address < 0xffffffff) {
            cout << "located file content in ROM at 0x" << hex << setfill('0') << setw(8) << address
                 << dec << endl
                 << flush;

            found = true;
        } else {
            break;
        }

        address += size;
    } while (address >= romStart && address < romStart + romSize);

    if (!found) cout << "unable to locate file" << endl << flush;
}
