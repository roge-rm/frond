// The Dana's host keyboard chords (DanaHostKeys.h).
#include <cstdio>
#include <string>

#include "DanaHostKeys.h"

static int failures = 0;

static std::string show(const std::vector<DanaHostChange>& changes) {
    std::string out;
    for (const auto& c : changes) out += (out.empty() ? "" : " ") + std::string(c.down ? "+" : "-") + c.name + (c.source == DanaSource5::physicalRight ? "/r" : c.source == DanaSource5::physicalPage ? "/p" : "");
    return out;
}

static void expect(const char* what, const std::vector<DanaHostChange>& got, const char* want) {
    if (show(got) != want) {
        std::printf("FAIL %s: got \"%s\", want \"%s\"\n", what, show(got).c_str(), want);
        ++failures;
    }
}

int main() {
    const auto L = SDL_SCANCODE_LCTRL, R = SDL_SCANCODE_RCTRL;
    {
        DanaHostKeys k;
        expect("ctrl down is held back", k.Event(L, true, "ctrl"), "");
        expect("ctrl+f1", k.Event(SDL_SCANCODE_F1, true, "f1"), "+calendar");
        expect("ctrl up after a chord", k.Event(L, false, "ctrl"), "");
        expect("f1 up releases calendar", k.Event(SDL_SCANCODE_F1, false, "f1"), "-calendar");
    }
    {
        DanaHostKeys k;
        k.Event(R, true, "ctrl");
        expect("right ctrl+esc", k.Event(SDL_SCANCODE_ESCAPE, true, "escape"), "+power");
        expect("esc up with ctrl still down", k.Event(SDL_SCANCODE_ESCAPE, false, "escape"), "-power");
        expect("right ctrl up", k.Event(R, false, "ctrl"), "");
    }
    {
        DanaHostKeys k;
        k.Event(L, true, "ctrl");
        expect("ctrl+a", k.Event(SDL_SCANCODE_A, true, "a"), "+ctrl +a");
        expect("a up", k.Event(SDL_SCANCODE_A, false, "a"), "-a");
        expect("ctrl up", k.Event(L, false, "ctrl"), "-ctrl");
    }
    {
        DanaHostKeys k;
        k.Event(R, true, "ctrl");
        expect("right ctrl+a", k.Event(SDL_SCANCODE_A, true, "a"), "+ctrl/r +a");
        expect("right ctrl up", k.Event(R, false, "ctrl"), "-ctrl/r");
    }
    {
        DanaHostKeys k;
        expect("ctrl tap down", k.Event(L, true, "ctrl"), "");
        expect("ctrl tap up", k.Event(L, false, "ctrl"), "+ctrl -ctrl");
    }
    {
        DanaHostKeys k;
        k.Event(L, true, "ctrl");
        k.Event(SDL_SCANCODE_F3, true, "f3");
        expect("ctrl up first", k.Event(L, false, "ctrl"), "");
        expect("f3 up", k.Event(SDL_SCANCODE_F3, false, "f3"), "-todo");
        expect("f3 plain", k.Event(SDL_SCANCODE_F3, true, "f3"), "+f3");
        expect("f3 plain up", k.Event(SDL_SCANCODE_F3, false, "f3"), "-f3");
    }
    {
        DanaHostKeys k;
        k.Event(SDL_SCANCODE_LALT, true, "function");
        k.Event(L, true, "ctrl");
        expect("alt+ctrl+f1", k.Event(SDL_SCANCODE_F1, true, "f1"), "+calendar");
        expect("alt+ctrl+f8", k.Event(SDL_SCANCODE_F8, true, "f8"), "+send");
    }
    {
        DanaHostKeys k;
        k.Event(L, true, "ctrl");
        k.Event(SDL_SCANCODE_A, true, "a");
        expect("ctrl already sent comes up first", k.Event(SDL_SCANCODE_F2, true, "f2"), "-ctrl +address");
        expect("ctrl up after that", k.Event(L, false, "ctrl"), "");
    }
    {
        DanaHostKeys k;
        expect("pgup", k.Event(SDL_SCANCODE_PAGEUP, true, "pageup"), "+function/p +up/p");
        expect("pgup up", k.Event(SDL_SCANCODE_PAGEUP, false, "pageup"), "-up/p -function/p");
        expect("pgdn", k.Event(SDL_SCANCODE_PAGEDOWN, true, "pagedown"), "+function/p +down/p");
        expect("pgdn up", k.Event(SDL_SCANCODE_PAGEDOWN, false, "pagedown"), "-down/p -function/p");
    }
    {
        DanaHostKeys k;
        expect("plain f1", k.Event(SDL_SCANCODE_F1, true, "f1"), "+f1");
        expect("plain esc", k.Event(SDL_SCANCODE_ESCAPE, true, "escape"), "+escape");
        expect("right alt", k.Event(SDL_SCANCODE_RALT, true, "function"), "+function/r");
        k.Event(L, true, "ctrl");
        k.Clear();
        expect("clear forgets ctrl", k.Event(L, false, "ctrl"), "");
        k.Event(L, true, "ctrl");
        k.Event(SDL_SCANCODE_F1, true, "f1");
        k.Clear();
        expect("clear forgets chords", k.Event(SDL_SCANCODE_F1, false, "f1"), "-f1");
    }
    std::printf(failures ? "dana-host-keys: FAIL\n" : "dana-host-keys: ok\n");
    return failures ? 1 : 0;
}
