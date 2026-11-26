// PDB and PRC files: install queue, backups and the sync tool.
#include "sync/PalmDb.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    frondSync::PalmDb db;
    std::string error;
    if (frondSync::PalmDb::Read(bytes, size, db, error)) {
        frondSync::Bytes out;
        db.Write(out, &error);
    }
    return 0;
}
