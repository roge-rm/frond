#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {

void Require(bool condition, const char* check) {
    if (!condition) {
        std::fprintf(stderr, "%s failed (%lu)\n", check, GetLastError());
        std::exit(1);
    }
}

template <typename Function>
Function Proc(HMODULE library, const char* name) {
    const auto function = GetProcAddress(library, name);
    Require(function != nullptr, name);
    return reinterpret_cast<Function>(function);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Require(argc == 3, "CondMgr.dll and installed conduit folder arguments");
    const auto library = LoadLibraryW(argv[1]);
    Require(library != nullptr, "load CondMgr.dll");
    auto stage = std::filesystem::absolute(argv[2]);
    Require(std::filesystem::create_directory(stage), "create private test directory");
    Require(SetEnvironmentVariableW(L"FROND_PALM_CONDUIT_STAGE", stage.c_str()), "set registration directory");
    const auto count = Proc<int (WINAPI*)()>(library, "CmGetConduitCount");
    const auto install = Proc<int (WINAPI*)(const char*, int)>(library, "CmInstallCreator");
    const auto remove = Proc<int (WINAPI*)(const char*)>(library, "CmRemoveConduitByCreatorID");
    const auto setName = Proc<int (WINAPI*)(const char*, const char*)>(library, "CmSetCreatorName");
    const auto getName = Proc<int (WINAPI*)(const char*, char*, int*)>(library, "CmGetCreatorName");
    const auto setTitle = Proc<int (WINAPI*)(const char*, const char*)>(library, "CmSetCreatorTitle");
    const auto getCreator = Proc<int (WINAPI*)(int, char*, int*)>(library, "CmGetConduitCreatorID");
    const auto getRegistration = Proc<int (WINAPI*)(const char*, HANDLE*)>(library, "CmGetConduitByCreator");
    const auto complete = Proc<int (WINAPI*)()>(library, "FrondCompleteRegistrations");
    Require(count() == 0, "empty directory");
    Require(install("Fron", 0) == -1015 && install("Frond", 1) == -1012, "reject unsupported type and creator");
    Require(install("Fron", 1) == 0 && install("Fron", 1) == -1005, "register exactly once");
    Require(count() == 1 && std::filesystem::is_regular_file(stage / "46726f6e.conf"), "registration persisted");
    const auto missing = (stage / "missing.dll").string();
    Require(setName("Fron", missing.c_str()) == 0 && complete() != 0, "accept the declaration but refuse to activate an absent DLL");
    const auto name = std::filesystem::absolute(argv[1]).string();
    Require(setName("Fron", name.c_str()) == 0 && complete() == 0, "activate the installed DLL");
    char buffer[MAX_PATH]{};
    int size = 0;
    Require(getName("Fron", buffer, &size) == -1010 && size == static_cast<int>(name.size() + 1), "report required name length");
    Require(getName("Fron", buffer, &size) == 0 && name == buffer, "get installed module");
    HANDLE global{};
    Require(getRegistration("Fron", &global) == 0 && global != nullptr, "get conduit descriptor");
    auto* description = static_cast<int*>(GlobalLock(global));
    Require(description && description[0] == 100 && description[2] == 1, "get conduit descriptor fields");
    Require(description[6] == 52 && name == reinterpret_cast<const char*>(description) + description[6],
            "descriptor contains original module");
    GlobalUnlock(global);
    GlobalFree(global);
    size = sizeof(buffer);
    Require(getCreator(0, buffer, &size) == 0 && std::strcmp(buffer, "Fron") == 0, "enumerate installed creator");
    Require(remove("Fron") == 1 && remove("Fron") == 0 && count() == 0, "remove only the named registration");
    const auto one = stage / "Programs";
    const auto two = stage / "Other Programs";
    const auto installDir = "Frond Fixture";
    const auto leaf = "FixtureConduit.dll";
    Require(std::filesystem::create_directories(one / installDir) &&
                std::filesystem::create_directories(two / installDir), "create fixture installation folders");
    Require(std::filesystem::copy_file(argv[1], one / installDir / leaf) &&
                std::filesystem::copy_file(argv[1], two / installDir / leaf), "publish fixture DLLs");
    Require(SetEnvironmentVariableW(L"ProgramFiles", one.c_str()) && SetEnvironmentVariableW(L"ProgramFiles(x86)", two.c_str()),
            "use exact fixture program locations");
    Require(install("Fron", 1) == 0 && setName("Fron", leaf) == 0 && setTitle("Fron", installDir) == 0,
            "declare a bare module name");
    Require(complete() != 0, "refuse ambiguous bare module name");
    Require(std::filesystem::remove(two / installDir / leaf) && complete() == 0, "resolve unique title folder");
    size = sizeof(buffer);
    Require(getName("Fron", buffer, &size) == 0 && (one / installDir / leaf).string() == buffer,
            "publish the exact installed module path");
    Require(remove("Fron") == 1 && std::filesystem::remove_all(stage) > 0, "remove private fixture directory");
    FreeLibrary(library);
    std::puts("Palm conduit registration checked");
    return 0;
}
