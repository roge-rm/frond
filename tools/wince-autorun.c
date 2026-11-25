// Windows Mobile runs a copy of the card's 2577\autorun.exe and passes "install".
typedef unsigned short Wide;
typedef unsigned long Dword;
typedef void* Handle;
typedef int Bool;

typedef struct {
    Handle process, thread;
    Dword processId, threadId;
} ProcessInfo;

extern const Wide* (*__imp_GetCommandLineW)(void);
#define GetCommandLineW (*__imp_GetCommandLineW)
extern Bool (*__imp_SHGetAutoRunPath)(Wide*);
#define SHGetAutoRunPath (*__imp_SHGetAutoRunPath)
extern Bool (*__imp_CopyFileW)(const Wide*, const Wide*, Bool);
#define CopyFileW (*__imp_CopyFileW)
extern Bool (*__imp_DeleteFileW)(const Wide*);
#define DeleteFileW (*__imp_DeleteFileW)
extern Dword (*__imp_WaitForSingleObject)(Handle, Dword);
#define WaitForSingleObject (*__imp_WaitForSingleObject)
extern Bool (*__imp_CreateProcessW)(const Wide*, Wide*, void*, void*, Bool, Dword, void*, const Wide*, void*, ProcessInfo*);
#define CreateProcessW (*__imp_CreateProcessW)
extern Bool (*__imp_CloseHandle)(Handle);
#define CloseHandle (*__imp_CloseHandle)

extern Dword (*__imp_GetTickCount)(void);
#define GetTickCount (*__imp_GetTickCount)
extern Dword (*__imp_GetLastError)(void);
#define GetLastError (*__imp_GetLastError)

static Wide pending[260], helper[260];
static Wide tempCab[] = L"\\Temp\\Frond00000000.cab";
static Wide arguments[40];

static unsigned length(const Wide* text) {
    unsigned n = 0;
    while (text[n]) ++n;
    return n;
}

static Bool append(Wide* dest, unsigned cap, const Wide* text) {
    unsigned n = length(dest);
    while (*text && n + 1 < cap) dest[n++] = *text++;
    dest[n] = 0;
    return !*text;
}

static Bool copyToTemp(void) {
    const Wide* digits = L"0123456789ABCDEF";
    const unsigned start = sizeof(L"\\Temp\\Frond") / sizeof(Wide) - 1;
    Dword serial = GetTickCount();
    for (unsigned attempt = 0; attempt < 32; ++attempt, ++serial) {
        for (unsigned digit = 0; digit < 8; ++digit)
            tempCab[start + digit] = digits[(serial >> ((7 - digit) * 4)) & 15];
        if (CopyFileW(pending, tempCab, 1)) return 1;
        const Dword error = GetLastError();
        if (error != 80 && error != 183) return 0;
    }
    return 0;
}

static Bool requested(void) {
    const Wide* command = GetCommandLineW();
    const Wide* end = command + length(command);
    while (end != command && end[-1] == ' ') --end;
    const Wide* verb = L"install";
    if (end - command < 7 || (end - command > 7 && end[-8] != ' ')) return 0;
    for (unsigned i = 0; i < 7; ++i)
        if ((end - 7)[i] != verb[i]) return 0;
    return 1;
}

int autorun(void) {
    if (!requested()) return 0;
    if (!SHGetAutoRunPath(helper)) return 0;
    // Keep the card's mount name and directory. The shell runs a copy of the helper.
    unsigned prefix = length(helper);
    while (prefix && helper[prefix - 1] != '\\') --prefix;
    if (!prefix) return 1;
    for (unsigned i = 0; i < prefix; ++i) pending[i] = helper[i];
    if (!append(pending, 260, L"FrondPending.cab") || !copyToTemp()) return 0;
    // Consume the request on the card before starting the installer from internal storage.
    if (!DeleteFileW(pending)) {
        DeleteFileW(tempCab);
        return 1;
    }

    append(arguments, 40, L"\"");
    append(arguments, 40, tempCab);
    append(arguments, 40, L"\" /nodelete");
    DeleteFileW(helper);
    ProcessInfo process;
    if (!CreateProcessW(L"\\Windows\\wceload.exe", arguments, 0, 0, 0, 0, 0, 0, 0, &process)) {
        DeleteFileW(tempCab);
        return 1;
    }
    CloseHandle(process.thread);
    WaitForSingleObject(process.process, (Dword)-1);
    CloseHandle(process.process);
    DeleteFileW(tempCab);
    return 0;
}
