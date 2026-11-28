// The Android sync manager's request handler (SyncJni.cpp run()), called without a JVM.
#include "../android/app/src/main/cpp/SyncJni.cpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

struct Result {
    bool ok;
    std::string error;
    std::string json;
};

Result call(const std::string& root, const std::string& request) {
    DynamicJsonDocument in(8192);
    if (deserializeJson(in, request)) { std::fprintf(stderr, "bad test request: %s\n", request.c_str()); std::exit(2); }
    DynamicJsonDocument out(16384);
    Result result;
    result.ok = run(root, in.as<JsonObjectConst>(), out.to<JsonObject>(), result.error);
    serializeJson(out, result.json);
    return result;
}

bool has(const Result& r, const std::string& text) { return r.json.find(text) != std::string::npos; }

void runCases(const std::string& root) {
    auto created = call(root, R"({"op":"create","name":"Test"})");
    check(created.ok, "create profile");
    DynamicJsonDocument made(256);
    deserializeJson(made, created.json);
    const auto id = made["id"].as<int64_t>();
    check(id > 0, "create answers an id");
    const auto idText = std::to_string(id);

    // The profiles op without a device key answers and has no "assigned".
    auto profiles = call(root, R"({"op":"profiles"})");
    check(profiles.ok, "profiles without a device key works");
    check(has(profiles, "\"rows\""), "profiles lists rows");
    check(!has(profiles, "\"assigned\""), "profiles without a device key has no assigned");

    check(call(root, R"({"op":"assign","device":"palm-1","profile":)" + idText + R"(,"kind":"palm"})").ok, "assign palm");
    check(call(root, R"({"op":"assign","device":"ce-1","profile":)" + idText + R"(,"kind":"winmo"})").ok, "assign winmo");

    profiles = call(root, R"({"op":"profiles","device":"palm-1"})");
    check(profiles.ok && has(profiles, "\"assigned\":" + idText), "profiles with a device key gives assigned");

    const std::string hot = R"({"op":"hotsync-transport","device":"palm-1","profile":)" + idText;
    auto r = call(root, hot + "}");
    check(r.ok && has(r, "\"connection\":\"local\""), "hotsync read with no value answers local");
    r = call(root, hot + R"(,"value":"network"})");
    check(r.ok, "hotsync set network");
    r = call(root, hot + "}");
    check(r.ok && has(r, "\"connection\":\"network\""), "hotsync read answers network");
    r = call(root, hot + R"(,"value":"bogus"})");
    check(!r.ok && r.error == "Choose USB or serial, or Network.", "hotsync refuses a bad value");
    r = call(root, hot + R"(,"value":"local"})");
    check(r.ok, "hotsync set local");
    r = call(root, hot + "}");
    check(r.ok && has(r, "\"connection\":\"local\""), "hotsync read answers local again");

    const std::string act = R"({"op":"activesync-transport","device":"ce-1","profile":)" + idText;
    r = call(root, act + "}");
    check(r.ok && has(r, "\"connection\":\"usb\""), "activesync read with no value answers usb");
    r = call(root, act + R"(,"value":"serial"})");
    check(r.ok, "activesync set serial");
    r = call(root, act + "}");
    check(r.ok && has(r, "\"connection\":\"serial\""), "activesync read answers serial");
    r = call(root, act + R"(,"value":"network"})");
    check(!r.ok && r.error == "Choose USB or serial.", "activesync refuses a bad value");

    // Other ops with optional keys left out.
    r = call(root, R"({"op":"actions","device":"palm-1","profile":)" + idText + "}");
    check(r.ok && has(r, "contacts"), "actions answers");
    r = call(root, R"({"op":"queue","profile":)" + idText + "}");
    check(r.ok && has(r, "\"rows\":[]"), "queue with no kind lists nothing");
    r = call(root, R"({"op":"log","profile":)" + idText + "}");
    check(r.ok && has(r, "\"rows\""), "log answers");
    r = call(root, R"({"op":"assign","device":"palm-1","profile":0})");
    check(r.ok, "assign with profile 0 unassigns");
    r = call(root, hot + "}");
    check(!r.ok && r.error == "Assign the device to this profile first.", "hotsync after unassign is refused");
    r = call(root, R"({"op":"nonsense"})");
    check(!r.ok, "unknown op without a profile is refused");
}
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s scratch-dir\n", argv[0]); return 2; }
    // A fresh root under the given folder, emptied again afterwards.
    const auto root = std::filesystem::path(argv[1]) / "root";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    runCases(root.string());
    std::filesystem::remove_all(root, ignored);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("sync-jni: all passed");
    return 0;
}
