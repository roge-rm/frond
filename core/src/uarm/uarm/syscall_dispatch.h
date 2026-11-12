#ifndef _SYSCALL_DISPATCH_H_
#define _SYSCALL_DISPATCH_H_

#include <cstdint>
#include <cstddef>
#include <functional>

#define SC_EXECUTE_PURE 0
#define SC_EXECUTE_FULL 1
// Filesystem calls may wait for an original OS task awakened by timer interrupts.
#define SC_EXECUTE_TIMERS 2

#ifndef __EMSCRIPTEN__
#include <stdexcept>
// Native read-only operations can roll back the complete guest on timeout.
class InjectedCallError : public std::runtime_error {
 public:
    InjectedCallError() : std::runtime_error("injected call timed out") {}
};
void syscallDispatchBeginRecovery(struct SyscallDispatch* sd);
void syscallDispatchEndRecovery(struct SyscallDispatch* sd);
void syscallDispatchValidateCommit(struct SyscallDispatch* sd);
bool syscallDispatchRecoveryActive(struct SyscallDispatch* sd);
void syscallDispatchBeginFileTransfer(struct SyscallDispatch* sd, uint32_t bytes);
void syscallDispatchFileProgress(struct SyscallDispatch* sd, uint32_t bytes);
#endif

struct SyscallDispatch;
struct ArmCpu;
class SoC;

struct SyscallDispatch* initSyscallDispatch(class SoC* cpu);

bool syscallDispatchInProgress(struct SyscallDispatch* sd);

bool syscallDispatchM68kSupport(struct SyscallDispatch* sd);
// WAKE: if the guest doesn't reach its event wait by itself, wake the UI task once (host calls).
// LOOK: only run the guest towards the wait, no wake (readiness checks).
// AFTER_NATIVE: NativeBoundary::Prepare has just run and woken the UI task, so no second wake and a
// short wait for the 68k side.
enum SyscallPrepareMode { SYSCALL_PREPARE_WAKE, SYSCALL_PREPARE_LOOK, SYSCALL_PREPARE_AFTER_NATIVE };
bool syscallDispatchPrepare(struct SyscallDispatch* sd, SyscallPrepareMode mode = SYSCALL_PREPARE_WAKE);

void syscallDispatchValidateMemory(struct SyscallDispatch*, uint32_t address, size_t size, bool write = true);

bool syscallDispatch_strncpy_toHost(struct SyscallDispatch* sd, void* dest, uint32_t src,
                                    size_t maxlen);
bool syscallDispatch_memcpy_fromHost(struct SyscallDispatch* sd, uint32_t dest, const void* src,
                                     size_t size);
bool syscallDispatch_memcpy_toHost(struct SyscallDispatch* sd, void* dest, uint32_t src,
                                   size_t size);

uint16_t syscall_SysSetAutoOffTime(struct SyscallDispatch* sd, uint32_t flags, uint32_t timeout);

uint16_t syscall_MemPtrNew(struct SyscallDispatch* sd, uint32_t flags, uint32_t size);

uint16_t syscall_MemPtrFree(struct SyscallDispatch* sd, uint32_t flags, uint32_t ptr);

uint16_t syscall_FtrGet(struct SyscallDispatch* sd, uint32_t flags, uint32_t creator,
                        uint16_t ftrNum, uint32_t valueP);

uint16_t syscall_FtrSet(struct SyscallDispatch* sd, uint32_t flags, uint32_t creator,
                        uint16_t ftrNum, uint32_t value);

uint32_t syscall68k_SysGetOsVersionString(struct SyscallDispatch* sd, uint32_t flags);
uint32_t syscall68k_MemPtrNew(struct SyscallDispatch* sd, uint32_t flags, uint32_t size);
uint16_t syscall68k_MemPtrFree(struct SyscallDispatch* sd, uint32_t flags, uint32_t memPtr);
uint16_t syscall68k_MemNumCards(struct SyscallDispatch* sd, uint32_t flags);
uint16_t syscall68k_ExgDBRead(struct SyscallDispatch* sd, uint32_t flags, uint32_t readProcP,
                              uint32_t deleteProcP, uint32_t userDataP, uint32_t dbIDP,
                              uint16_t cardNo, uint32_t needsResetP, bool keepDates);
uint16_t syscall68k_ExgDBWrite(struct SyscallDispatch* sd, uint32_t flags, uint32_t writeProcP,
                               uint32_t userDataP, uint32_t nameP, uint32_t dbID, uint16_t cardNo);
uint16_t syscall68k_DmNumDatabases(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo);
uint32_t syscall68k_DmGetDatabase(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                  uint16_t index);
uint16_t syscall68k_DmDatabaseInfo(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                   uint32_t dbId, uint32_t nameP, uint32_t attributesP,
                                   uint32_t versionP, uint32_t crDateP, uint32_t modDateP,
                                   uint32_t bcpUpDateP, uint32_t modNumP, uint32_t appInfoIDP,
                                   uint32_t sortInfoIDP, uint32_t typeP, uint32_t creatorP);
uint16_t syscall68k_DmDatabaseProtect(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                      uint32_t dbID, uint8_t protect);
uint16_t syscall68k_DmDeleteDatabase(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                     uint32_t dbID);
uint32_t syscall68k_DmFindDatabase(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                   uint32_t nameP);
uint16_t syscall68k_SysUIAppSwitch(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo,
                                   uint32_t dbID, uint16_t cmd, uint32_t cmdPBP);
uint16_t syscall68k_SysCurAppDatabase(struct SyscallDispatch* sd, uint32_t flags, uint32_t cardNoP,
                                      uint32_t dbIDP);
uint16_t syscall68k_SysAppLaunch(struct SyscallDispatch* sd, uint32_t flags, uint16_t cardNo, uint32_t dbID,
                                 uint16_t launchFlags, uint16_t cmd, uint32_t cmdPBP, uint32_t resultP);
uint16_t syscall68k_DmGetNextDatabaseByTypeCreator(struct SyscallDispatch* sd, uint32_t flags, bool newSearch,
                                                   uint32_t stateP, uint32_t type, uint32_t creator,
                                                   bool onlyLatestVersion, uint32_t cardNoP, uint32_t dbIDP);
uint16_t syscall68k_SysNotifyBroadcast(struct SyscallDispatch* sd, uint32_t flags, uint32_t notifyP);
bool syscall68k_EvtEventAvail(struct SyscallDispatch* sd, uint32_t flags);
bool syscall68k_EvtSysEventAvail(struct SyscallDispatch* sd, uint32_t flags, bool ignorePenUps);

using M68kStub = std::function<void(struct ArmCpu*, uint32_t parameterBase,
                                    std::function<void()> deadMansSwitch)>;
void syscallDispatchRegisterM68kStub(struct SyscallDispatch* sd, uint32_t trampoline,
                                     M68kStub stub, uint32_t argumentBytes = 0);
void syscallDispatchUnregisterM68kStub(struct SyscallDispatch* sd, uint32_t trampoline);

uint16_t syscall68k_VFSVolumeEnumerate(SyscallDispatch*, uint32_t flags, uint32_t volumeP, uint32_t iteratorP);
uint16_t syscall68k_VFSVolumeInfo(SyscallDispatch*, uint32_t flags, uint16_t volume, uint32_t infoP);
uint16_t syscall68k_VFSVolumeGetLabel(SyscallDispatch*, uint32_t flags, uint16_t volume, uint32_t labelP, uint16_t length);
uint16_t syscall68k_VFSVolumeSize(SyscallDispatch*, uint32_t flags, uint16_t volume, uint32_t usedP, uint32_t totalP);
uint16_t syscall68k_VFSVolumeFormat(SyscallDispatch*, uint32_t flags, uint8_t formatFlags, uint16_t fsLibrary, uint32_t mountParametersP);
uint16_t syscall68k_VFSFileCreate(SyscallDispatch*, uint32_t flags, uint16_t volume, uint32_t pathP);
uint16_t syscall68k_VFSFileOpen(SyscallDispatch*, uint32_t flags, uint16_t volume, uint32_t pathP, uint16_t mode, uint32_t fileP);
uint16_t syscall68k_VFSFileClose(SyscallDispatch*, uint32_t flags, uint32_t file);
uint16_t syscall68k_VFSFileRead(SyscallDispatch*, uint32_t flags, uint32_t file, uint32_t length, uint32_t bytesP, uint32_t countP);
uint16_t syscall68k_VFSFileWrite(SyscallDispatch*, uint32_t flags, uint32_t file, uint32_t length, uint32_t bytesP, uint32_t countP);
uint16_t syscall68k_VFSFileDelete(SyscallDispatch*, uint32_t flags, uint16_t volume, uint32_t pathP);
uint16_t syscall68k_VFSFileRename(SyscallDispatch*, uint32_t flags, uint16_t volume, uint32_t pathP, uint32_t nameP);
uint16_t syscall68k_VFSFileSize(SyscallDispatch*, uint32_t flags, uint32_t file, uint32_t sizeP);
uint16_t syscall68k_VFSDirCreate(SyscallDispatch*, uint32_t flags, uint16_t volume, uint32_t pathP);
uint16_t syscall68k_VFSDirEntryEnumerate(SyscallDispatch*, uint32_t flags, uint32_t directory, uint32_t iteratorP, uint32_t infoP);

uint16_t syscall68k_VFSFileGetAttributes(SyscallDispatch*, uint32_t flags, uint32_t file, uint32_t attributesP);

#endif  // _SYSCALL_DISPATCH_H_
