#include <cassert>
#include <cstdio>
#include <cstdlib>
extern "C" void uarmAbort() { std::abort(); }
#include <cstring>
#include "db_backup.h"
#include "syscall_dispatch.h"

static bool allocationFails, copyFails, includeOpen;
static uint16_t apiError;
static uint32_t result;
static M68kStub stub;
bool dbListGet(SyscallDispatch*, DbMetadataList& list) {
    list.resize(includeOpen?2:1); auto& db = list[0];
    db.localID=1;
    if(includeOpen){auto& opened=list[1];std::strcpy(opened.name,"Shim Logs");opened.localID=2;opened.isRom=false;}
    std::strcpy(db.name, "AciduliteLog");
    db.attr = 8; db.creator = 0x526d7266; db.isRom = false;
    return true;
}
bool syscallDispatchM68kSupport(SyscallDispatch*) { return true; }
bool syscallDispatchPrepare(SyscallDispatch*) { return true; }
uint32_t syscall68k_MemPtrNew(SyscallDispatch*, uint32_t, uint32_t) {
    return allocationFails ? 0 : 0x1000;
}
uint16_t syscall68k_MemPtrFree(SyscallDispatch*, uint32_t, uint32_t p) {
    assert(p == 0x1000); return 0;
}
void paceSet8(uint32_t p, uint8_t) { assert(p == 0x1002); }
void paceSetDreg(uint8_t reg, uint32_t value) { assert(reg == 0); result = value; }
uint32_t paceGet32(uint32_t p) {
    if (p == 0x2000) return 0x3000;
    if (p == 0x2004) return 0x4000;
    assert(p == 0x4000); return 8;
}
bool syscallDispatch_memcpy_toHost(SyscallDispatch*, void* out, uint32_t p, size_t n) {
    assert(p == 0x3000 && n == 8);
    std::memset(out, 1, n); return !copyFails;
}
void syscallDispatchRegisterM68kStub(SyscallDispatch*, uint32_t, M68kStub fn) { stub = fn; }
void syscallDispatchUnregisterM68kStub(SyscallDispatch*, uint32_t) { stub = {}; }
uint16_t syscall68k_ExgDBWrite(SyscallDispatch*, uint32_t, uint32_t, uint32_t,
                              uint32_t, uint32_t id, uint16_t) {
    if(id==2)return 0x205;
    if (!apiError) stub(nullptr, 0x2000, []() {});
    return apiError;
}
int main() {
    for (unsigned failure = 0; failure < 4; ++failure) {
        allocationFails = failure == 1; copyFails = failure == 2;
        apiError = failure == 3 ? 1 : 0;
        DbBackup backup(nullptr, DbBackup::BACKUP_TYPE_RAM);
        assert(backup.Init());
        assert(backup.Continue() == (failure == 0));
        assert(!stub);
        assert(backup.GetState() == (failure ? DbBackup::STATE_ERROR : DbBackup::STATE_DONE));
        if (failure) assert(backup.GetArchiveData() == nullptr);
        else assert(result == 0 && backup.GetArchiveData() && backup.GetArchiveSize() > 0);
    }
    allocationFails=copyFails=false;apiError=0;includeOpen=true;
    DbBackup all(nullptr,DbBackup::BACKUP_TYPE_RAM);assert(all.Init());assert(all.Continue());
    assert(!all.Continue() && all.GetArchiveData()==nullptr); // retain all-or-nothing export
    DbBackup log(nullptr,DbBackup::BACKUP_TYPE_RAM,"AciduliteLog");assert(log.Init());
    while(log.GetState()==DbBackup::STATE_IN_PROGRESS)assert(log.Continue());
    assert(log.GetState()==DbBackup::STATE_DONE && log.GetArchiveData() && log.GetArchiveSize());
    DbBackup missing(nullptr,DbBackup::BACKUP_TYPE_RAM,"Absent");assert(!missing.Init());
    puts("PASS: database export rejects allocation, copy and Palm API failures without a partial archive");
}
