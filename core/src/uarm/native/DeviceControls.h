#ifndef FROND_DEVICE_CONTROLS_H
#define FROND_DEVICE_CONTROLS_H
#include <SDL.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "device_profile5.h"
#include "keys.h"

enum class DeviceControlIcon { home, calendar, contacts, web, up, down, left, right, select, power, tasks, notes,
                               media, music, phone, memo, graffiti, back, legend, stick, action, function,
                               trigger, bluetooth, inbox, start, ok, wireless, taskSwitch, today,
                               calendarGrid };
enum class DeviceControlStyle { tx, e2, zire72, m515, iiix, tg50, zodiac, ipaq210, rx1950, h4350, m125, v, m100, m500,
                                iiie, iiixe, vx, m105, c500, iiic, m505, m130, m520, t415 };
// Parts of a strip that can be hidden: the TG50's and h4350's keyboards and the TG50's side tab. A device with two
// arrangements, like the Zodiac, puts its keys under the screen (below) or in panels either side of
// it (around), and only the current arrangement's keys are shown.
enum class DeviceControlSection { main, keyboard, sideTab, below, around };
struct DeviceButton {
    KeyId key;
    SDL_Rect bounds;
    const char* label;
    DeviceControlIcon icon;
    // Corner radius in strip units; 0 takes the profile's.
    float corner{0};
    DeviceControlSection section{DeviceControlSection::main};
    // A key that is part of a bigger rounded shape, like half a rocker, hits where that shape
    // (with the corner above) and its bounds overlap; w 0 for none.
    SDL_Rect shape{0,0,0,0};
};
struct DeviceControlProfile {
    int width, height;
    const DeviceButton* buttons;
    size_t count;
    // Rounded five-way ring and its centre select pill, in strip coordinates; w 0 for none.
    SDL_Rect navigator;
    int navigatorCorner;
    SDL_Rect select;
    DeviceControlStyle style{DeviceControlStyle::tx};
    // App key corner radius as a fraction of the key's shorter side.
    float buttonCorner{0.3f};
    // Strip units per screen pixel. Art is authored 320 units wide, so 160-wide screens use 2.
    int unit{1};
    // Select key corner radius in strip units; 0 for a pill.
    float selectCorner{0};
    // Strip units at the bottom that belong to the keyboard; hiding it takes them off.
    int keyboardHeight{0};
    // Side panel left of the screen, and the strip units it adds to the window's width.
    SDL_Rect sideTab{0,0,0,0};
    int sideTabWidth{0};
    // Zodiac: width of each panel beside the screen in the around arrangement, and the panel
    // height its keys are placed in, in strip units.
    int panelWidth{0};
    int panelHeight{0};
    // Silkscreen units between the LCD and the strip. A device with a side tab and no keyboard
    // can put the tab beside the silkscreen and strip, which are then drawn narrower.
    int silkscreenHeight{0};
};
// Where a device with two arrangements puts its controls: in a strip under the screen, or in panels
// either side of it as on the real landscape Zodiac.
enum class DeviceControlArrangement { below, around };
// Which hideable parts of a strip are shown.
struct DeviceControlView {
    bool keyboard{true};
    bool sideTab{true};
    // Side tab under a full-width LCD: left of the keyboard, or on a device without one left of
    // the silkscreen and strip. Beside the LCD while the keyboard is hidden.
    bool sideTabBesideKeyboard{false};
    DeviceControlArrangement arrangement{DeviceControlArrangement::below};
};
// Where the CLIE jog controls go: hidden, left of the LCD, or left of the keyboard. keyboard is
// the saved name; on a CLIE without a keyboard it puts them beside the silkscreen and buttons.
enum class JogPlacement { hidden, screen, keyboard };
inline bool parseJogPlacement(const std::string& value, JogPlacement& placement) {
    if(value=="hidden")placement=JogPlacement::hidden;
    else if(value=="screen")placement=JogPlacement::screen;
    else if(value=="keyboard")placement=JogPlacement::keyboard;
    else return false;
    return true;
}
inline DeviceControlView deviceControlView(bool keyboard, JogPlacement jog) {
    return {keyboard,jog!=JogPlacement::hidden,jog==JogPlacement::keyboard};
}
// Pressed keys, one bit per KeyId.
__extension__ typedef unsigned __int128 DeviceKeyMask;
static_assert(keyIdCount<=128,"DeviceKeyMask has a bit per key");
// Analogue stick position, each axis -1 to 1 with right and down positive; 0,0 and not held when let go.
struct DeviceStick {
    float x{0}, y{0};
    bool held{false};
    bool operator==(const DeviceStick& o) const {return x==o.x && y==o.y && held==o.held;}
    bool operator!=(const DeviceStick& o) const {return !(*this==o);}
};
inline DeviceKeyMask deviceKeyBit(KeyId key) {return DeviceKeyMask(1)<<key;}
// Button bar and silkscreen art from the device table. Reserved names have art for tests and renders.
inline DeviceArt deviceArt(DeviceType5 device) { return deviceArtFor5(device); }
inline DeviceArt deviceArt(const char* name) {
    const auto* profile = deviceProfileNamed5(name);
    return profile ? profile->art : DeviceArt::none;
}
// h4350 thumb keyboard key n, 0-38 from the top left, in the keyboard id range.
constexpr KeyId h4350Key(int n) { return KeyId(keyIdKeyboard + n); }
inline const DeviceControlProfile& deviceControls(DeviceArt art) {
    // App keys hit their whole pocket; the five-way entries place the direction parts. The TX's
    // five-way nearly fills the bar's height, with each app key in its own pocket either side.
    static const DeviceButton tx[] = {
        {keyIdHard1, {4,25,44,40}, "Home", DeviceControlIcon::home},
        {keyIdHard2, {52,25,44,40}, "Calendar", DeviceControlIcon::calendar},
        {keyIdHard3, {224,25,44,40}, "Contacts", DeviceControlIcon::contacts},
        {keyIdHard4, {272,25,44,40}, "Web", DeviceControlIcon::web},
        {keyIdUp, {150,9,20,14}, "Up", DeviceControlIcon::up},
        {keyIdDown, {150,61,20,14}, "Down", DeviceControlIcon::down},
        {keyIdLeft, {111,32,18,20}, "Left", DeviceControlIcon::left},
        {keyIdRight, {191,32,18,20}, "Right", DeviceControlIcon::right},
        {keyIdSelect, {131,28,58,28}, "Select", DeviceControlIcon::select},
    };
    static const DeviceControlProfile txProfile{320,84,tx,std::size(tx),{108,5,104,74},24,{131,28,58,28},
                                                DeviceControlStyle::tx,0.3f,1,10};
    // E2: app keys hard1-4 open Calendar, Contacts, Tasks and Note Pad, in pairs on one line either
    // side of a rounded five-way that fills most of the bar, with a rounded select key.
    static const DeviceButton e2[] = {
        {keyIdHard1, {4,30,44,36}, "Calendar", DeviceControlIcon::calendar},
        {keyIdHard2, {52,30,44,36}, "Contacts", DeviceControlIcon::contacts},
        {keyIdHard3, {224,30,44,36}, "Tasks", DeviceControlIcon::tasks},
        {keyIdHard4, {272,30,44,36}, "Note Pad", DeviceControlIcon::notes},
        {keyIdUp, {150,6,20,14}, "Up", DeviceControlIcon::up},
        {keyIdDown, {150,76,20,14}, "Down", DeviceControlIcon::down},
        {keyIdLeft, {104,38,18,20}, "Left", DeviceControlIcon::left},
        {keyIdRight, {198,38,18,20}, "Right", DeviceControlIcon::right},
        {keyIdSelect, {130,31,60,34}, "Select", DeviceControlIcon::select},
    };
    static const DeviceControlProfile e2Profile{320,96,e2,std::size(e2),{101,3,118,90},26,{130,31,60,34},
                                                DeviceControlStyle::e2,0.27f,1,12};
    // Zire 72: pill-shaped app keys in stacked pairs, Calendar over Contacts on the left and
    // Camera over Music on the right, round a rounded-square five-way with a rounded-rectangle
    // select key. The ROM opens RealOne on hard3 and Camera on hard4.
    static const DeviceButton zire72[] = {
        {keyIdHard1, {10,19,82,28}, "Calendar", DeviceControlIcon::calendar},
        {keyIdHard2, {10,57,82,28}, "Contacts", DeviceControlIcon::contacts},
        {keyIdHard3, {228,57,82,28}, "Music", DeviceControlIcon::music},
        {keyIdHard4, {228,19,82,28}, "Camera", DeviceControlIcon::media},
        {keyIdUp, {150,15,20,14}, "Up", DeviceControlIcon::up},
        {keyIdDown, {150,76,20,12}, "Down", DeviceControlIcon::down},
        {keyIdLeft, {108,42,18,20}, "Left", DeviceControlIcon::left},
        {keyIdRight, {194,42,18,20}, "Right", DeviceControlIcon::right},
        {keyIdSelect, {138,36,44,32}, "Select", DeviceControlIcon::select},
    };
    static const DeviceControlProfile zire72Profile{320,104,zire72,std::size(zire72),{105,13,110,78},25,{138,36,44,32},
                                                    DeviceControlStyle::zire72,0.5f,1,10};
    // m515 on a 160-wide screen, 2 units per pixel: four round app keys (Date Book, Address,
    // To Do, Note Pad), the outer ones higher, either side of two round scroll keys stacked in a
    // pill-shaped housing, on a tall chin. No five-way, so up and down hit as their own round keys.
    static const DeviceButton m515[] = {
        {keyIdHard1, {4,14,56,56}, "Date Book", DeviceControlIcon::calendar},
        {keyIdHard2, {70,22,56,56}, "Address", DeviceControlIcon::phone},
        {keyIdHard3, {194,22,56,56}, "To Do", DeviceControlIcon::tasks},
        {keyIdHard4, {260,14,56,56}, "Note Pad", DeviceControlIcon::notes},
        {keyIdUp, {145,15,30,30}, "Up", DeviceControlIcon::up},
        {keyIdDown, {145,47,30,30}, "Down", DeviceControlIcon::down},
    };
    static const DeviceControlProfile m515Profile{320,88,m515,std::size(m515),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::m515,0.5f,2};
    // Palm IIIx on a 160-wide screen, 2 units per pixel: four round app keys in dark oval
    // pockets (Date Book, Address, To Do, Memo Pad), the outer ones a hair higher, round a
    // peanut-shaped scroll rocker that rocks up and down; its halves meet across the whole
    // drawn waist. The teal power key sits at the far left; the real one is outside the
    // screen's width, so the row is pulled in to fit.
    static const DeviceButton iiix[] = {
        {keyIdHard1, {19,20,50,44}, "Date Book", DeviceControlIcon::calendar},
        {keyIdHard2, {78,21,50,44}, "Address", DeviceControlIcon::phone},
        {keyIdHard3, {192,21,50,44}, "To Do", DeviceControlIcon::tasks},
        {keyIdHard4, {251,20,50,44}, "Memo Pad", DeviceControlIcon::memo},
        {keyIdUp, {144,8,32,37}, "Up", DeviceControlIcon::up, 8},
        {keyIdDown, {144,45,32,37}, "Down", DeviceControlIcon::down, 8},
        {keyIdPower, {2,32,13,38}, "Power", DeviceControlIcon::power, 3},
    };
    static const DeviceControlProfile iiixProfile{320,88,iiix,std::size(iiix),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::iiix,0.5f,2};
    // Palm IIIe and IIIxe: the IIIx's bar, each in its own shade of grey.
    static const DeviceControlProfile iiieProfile{320,88,iiix,std::size(iiix),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::iiie,0.5f,2};
    static const DeviceControlProfile iiixeProfile{320,88,iiix,std::size(iiix),{0,0,0,0},0,{0,0,0,0},
                                                   DeviceControlStyle::iiixe,0.5f,2};
    // Palm m125 on a 160-wide screen, 2 units per pixel: four round black keys sunk in pockets in
    // the blue-grey face (Date Book, Address, To Do, Note Pad), the outer ones higher, either side
    // of two black scroll keys in a peanut-shaped well. Each app key hits its whole pocket. The
    // real outer keys sit wider than the LCD, so the row is pulled in to fit. Power is on the top
    // edge, not on this face.
    static const DeviceButton m125[] = {
        {keyIdHard1, {10,14,48,48}, "Date Book", DeviceControlIcon::calendar},
        {keyIdHard2, {72,23,48,48}, "Address", DeviceControlIcon::phone},
        {keyIdHard3, {200,23,48,48}, "To Do", DeviceControlIcon::tasks},
        {keyIdHard4, {262,14,48,48}, "Note Pad", DeviceControlIcon::notes},
        {keyIdUp, {145,14,30,29}, "Up", DeviceControlIcon::up, 9},
        {keyIdDown, {145,49,30,29}, "Down", DeviceControlIcon::down, 9},
    };
    static const DeviceControlProfile m125Profile{320,88,m125,std::size(m125),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::m125,0.5f,2};
    // Palm m100 and m105: the m125's keys and scroll well on charcoal plastic, the m105 a lighter
    // grey, with plain scroll keys.
    static const DeviceControlProfile m100Profile{320,88,m125,std::size(m125),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::m100,0.5f,2};
    static const DeviceControlProfile m105Profile{320,88,m125,std::size(m125),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::m105,0.5f,2};
    // Palm V and Vx on a 160-wide screen, 2 units per pixel: four small black oval keys, a little
    // wider than tall, in thin collars in the metal face (Date Book, Address, To Do, Memo Pad), the
    // outer ones higher, either side of one narrow, tall black rocker with a slight waist that
    // rocks up and down. The real outer keys sit wider than the LCD, so the row is pulled in to fit. Power
    // is on the top edge, not on this face.
    static const DeviceButton v[] = {
        {keyIdHard1, {20,27,38,32}, "Date Book", DeviceControlIcon::calendar},
        {keyIdHard2, {84,35,38,32}, "Address", DeviceControlIcon::phone},
        {keyIdHard3, {198,35,38,32}, "To Do", DeviceControlIcon::tasks},
        {keyIdHard4, {262,27,38,32}, "Memo Pad", DeviceControlIcon::memo},
        {keyIdUp, {144,12,32,36}, "Up", DeviceControlIcon::up, 16, DeviceControlSection::main, {144,12,32,72}},
        {keyIdDown, {144,48,32,36}, "Down", DeviceControlIcon::down, 16, DeviceControlSection::main, {144,12,32,72}},
    };
    static const DeviceControlProfile vProfile{320,96,v,std::size(v),{0,0,0,0},0,{0,0,0,0},
                                               DeviceControlStyle::v,0.5f,2};
    // Palm Vx: the V's bar in a lighter silver.
    static const DeviceControlProfile vxProfile{320,96,v,std::size(v),{0,0,0,0},0,{0,0,0,0},
                                                DeviceControlStyle::vx,0.5f,2};
    // Palm m500 and WorkPad c500: the m515's shape with black keys, on blue-grey silver and on black.
    static const DeviceControlProfile m500Profile{320,88,m515,std::size(m515),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::m500,0.5f,2};
    static const DeviceControlProfile c500Profile{320,88,m515,std::size(m515),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::c500,0.5f,2};
    // Palm m505 and m520: the m515's keys, on a lighter silver and on gunmetal.
    static const DeviceControlProfile m505Profile{320,88,m515,std::size(m515),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::m505,0.5f,2};
    static const DeviceControlProfile m520Profile{320,88,m515,std::size(m515),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::m520,0.5f,2};
    // Palm m130: the m125's keys on a pale silver face with a blue edge.
    static const DeviceControlProfile m130Profile{320,88,m125,std::size(m125),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::m130,0.5f,2};
    // Palm IIIc on a 160-wide screen, 2 units per pixel: the teal power key at the far left, then
    // four dark oval keys in matte grey collars (Date Book, Address, To Do, Memo Pad) in a straight
    // row, either side of two small separate oval scroll keys, one over the other.
    static const DeviceButton iiic[] = {
        {keyIdHard1, {22,24,48,44}, "Date Book", DeviceControlIcon::calendar},
        {keyIdHard2, {80,24,48,44}, "Address", DeviceControlIcon::phone},
        {keyIdHard3, {192,24,48,44}, "To Do", DeviceControlIcon::tasks},
        {keyIdHard4, {250,24,48,44}, "Memo Pad", DeviceControlIcon::memo},
        {keyIdUp, {147,25,26,16}, "Up", DeviceControlIcon::up, 8},
        {keyIdDown, {147,47,26,16}, "Down", DeviceControlIcon::down, 8},
        {keyIdPower, {2,32,13,38}, "Power", DeviceControlIcon::power, 3},
    };
    static const DeviceControlProfile iiicProfile{320,88,iiic,std::size(iiic),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::iiic,0.5f,2};
    // CLIE PEG-TG50: round Home and Graffiti keys at the ends of the app row, five slim keys in
    // a raised bar between them (Date Book, Address, the scroll rocker split across its middle,
    // To Do, Memo Pad), then the QWERTY keyboard. The Jog Dial and Back sit beside
    // the middle of the LCD on the left side of the body.
    constexpr auto kb=DeviceControlSection::keyboard;
    constexpr auto tab=DeviceControlSection::sideTab;
    static const DeviceButton tg50[] = {
        {keyIdHome, {6,18,24,24}, "Home", DeviceControlIcon::home, 12},
        {keyIdHard1, {42,22,44,16}, "Date Book", DeviceControlIcon::calendar, 8},
        {keyIdHard2, {90,22,44,16}, "Address", DeviceControlIcon::phone, 8},
        {keyIdUp, {138,22,44,8}, "Up", DeviceControlIcon::up, 8, DeviceControlSection::main, {138,22,44,16}},
        {keyIdDown, {138,30,44,8}, "Down", DeviceControlIcon::down, 8, DeviceControlSection::main, {138,22,44,16}},
        {keyIdHard3, {186,22,44,16}, "To Do", DeviceControlIcon::tasks, 8},
        {keyIdHard4, {234,22,44,16}, "Memo Pad", DeviceControlIcon::memo, 8},
        {keyIdGraffiti, {290,18,24,24}, "Graffiti", DeviceControlIcon::graffiti, 12},
        // Keys 27x24 on a 31-unit pitch, rows 34 apart; each row below starts half a key in. Ids
        // follow the ROM's key matrix; the modifier column is Caps, Tab, Shift, Ctrl, Blue, Red
        // on drives 2-7, as in uARM's TG50 model.
        {tg50Key(2,1), {7,52,27,24}, "Q", DeviceControlIcon::legend, 0, kb},
        {tg50Key(2,2), {38,52,27,24}, "W", DeviceControlIcon::legend, 0, kb},
        {tg50Key(2,3), {69,52,27,24}, "E", DeviceControlIcon::legend, 0, kb},
        {tg50Key(2,4), {100,52,27,24}, "R", DeviceControlIcon::legend, 0, kb},
        {tg50Key(2,5), {131,52,27,24}, "T", DeviceControlIcon::legend, 0, kb},
        {tg50Key(3,1), {162,52,27,24}, "Y", DeviceControlIcon::legend, 0, kb},
        {tg50Key(3,2), {193,52,27,24}, "U", DeviceControlIcon::legend, 0, kb},
        {tg50Key(3,3), {224,52,27,24}, "I", DeviceControlIcon::legend, 0, kb},
        {tg50Key(3,4), {255,52,27,24}, "O", DeviceControlIcon::legend, 0, kb},
        {tg50Key(3,5), {286,52,27,24}, "P", DeviceControlIcon::legend, 0, kb},
        {tg50Key(4,1), {23,86,27,24}, "A", DeviceControlIcon::legend, 0, kb},
        {tg50Key(4,2), {54,86,27,24}, "S", DeviceControlIcon::legend, 0, kb},
        {tg50Key(4,3), {85,86,27,24}, "D", DeviceControlIcon::legend, 0, kb},
        {tg50Key(4,4), {116,86,27,24}, "F", DeviceControlIcon::legend, 0, kb},
        {tg50Key(4,5), {147,86,27,24}, "G", DeviceControlIcon::legend, 0, kb},
        {tg50Key(2,6), {178,86,27,24}, "H", DeviceControlIcon::legend, 0, kb},
        {tg50Key(3,6), {209,86,27,24}, "J", DeviceControlIcon::legend, 0, kb},
        {tg50Key(4,6), {240,86,27,24}, "K", DeviceControlIcon::legend, 0, kb},
        {tg50Key(5,6), {271,86,27,24}, "L", DeviceControlIcon::legend, 0, kb},
        {tg50Key(3,0), {7,120,43,24}, "Tab", DeviceControlIcon::legend, 0, kb},
        {tg50Key(5,1), {54,120,27,24}, "Z", DeviceControlIcon::legend, 0, kb},
        {tg50Key(5,2), {85,120,27,24}, "X", DeviceControlIcon::legend, 0, kb},
        {tg50Key(5,3), {116,120,27,24}, "C", DeviceControlIcon::legend, 0, kb},
        {tg50Key(5,4), {147,120,27,24}, "V", DeviceControlIcon::legend, 0, kb},
        {tg50Key(5,5), {178,120,27,24}, "B", DeviceControlIcon::legend, 0, kb},
        {tg50Key(6,1), {209,120,27,24}, "N", DeviceControlIcon::legend, 0, kb},
        {tg50Key(6,2), {240,120,27,24}, "M", DeviceControlIcon::legend, 0, kb},
        {tg50Key(7,6), {271,120,42,24}, "Backspace", DeviceControlIcon::legend, 0, kb},
        {tg50Key(5,0), {20,154,41,24}, "Ctrl", DeviceControlIcon::legend, 0, kb},
        {tg50Key(6,0), {65,154,47,24}, "Blue shift", DeviceControlIcon::legend, 0, kb},
        {tg50Key(7,2), {116,154,27,24}, "Minus", DeviceControlIcon::legend, 0, kb},
        {tg50Key(6,3), {147,154,27,24}, "Comma", DeviceControlIcon::legend, 0, kb},
        {tg50Key(6,4), {178,154,27,24}, "Full stop", DeviceControlIcon::legend, 0, kb},
        {tg50Key(6,5), {209,154,27,24}, "Up", DeviceControlIcon::legend, 0, kb},
        {tg50Key(7,5), {240,154,58,24}, "Enter", DeviceControlIcon::legend, 0, kb},
        {tg50Key(4,0), {20,188,41,24}, "Shift", DeviceControlIcon::legend, 0, kb},
        {tg50Key(7,0), {65,188,47,24}, "Red shift", DeviceControlIcon::legend, 0, kb},
        {tg50Key(7,1), {116,188,58,24}, "Space", DeviceControlIcon::legend, 0, kb},
        {tg50Key(7,3), {178,188,27,24}, "Left", DeviceControlIcon::legend, 0, kb},
        {tg50Key(6,6), {209,188,27,24}, "Down", DeviceControlIcon::legend, 0, kb},
        {tg50Key(7,4), {240,188,27,24}, "Right", DeviceControlIcon::legend, 0, kb},
        {tg50Key(2,0), {271,188,27,24}, "Caps lock", DeviceControlIcon::legend, 0, kb},
        // Side tab: the Jog Dial rolls up and down either side of its press, then Back.
        {keyIdJogUp, {-34,-230,30,26}, "Jog up", DeviceControlIcon::up, 6, tab},
        {keyIdJogPress, {-34,-198,30,64}, "Jog press", DeviceControlIcon::select, 6, tab},
        {keyIdJogDown, {-34,-128,30,26}, "Jog down", DeviceControlIcon::down, 6, tab},
        {keyIdBack, {-34,-86,30,26}, "Back", DeviceControlIcon::back, 6, tab},
    };
    static const DeviceControlProfile tg50Profile{320,224,tg50,std::size(tg50),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::tg50,0.3f,1,0,176,{-38,-236,38,208},38};
    // Tapwave Zodiac: power, the analogue stick, Function and Home on the left and four silver
    // Action keys in a round recess on the right, the shoulder triggers above them and the small
    // round Bluetooth button. Action keys are named by place; which of hard1-4 each one sends
    // isn't known yet. Each Action key's shape is the cluster's recess; it hits in its own quarter
    // of it (deviceClusterKeyAt). The stick entries place its well and carry its press.
    // Below: one strip under the screen, scaled to the screen's width. Around: a panel either side
    // of the screen, each 150 wide and authored 320 tall; the right panel's keys are authored
    // from x 150 and move past the screen. On a taller screen the triggers stay at the top and
    // the rest are centred.
    constexpr auto below=DeviceControlSection::below;
    constexpr auto around=DeviceControlSection::around;
    constexpr auto action=DeviceControlIcon::action;
    static const DeviceButton zodiac[] = {
        {keyIdTriggerLeft, {8,8,120,24}, "Left trigger", DeviceControlIcon::trigger, 10, below},
        {keyIdTriggerRight, {352,8,120,24}, "Right trigger", DeviceControlIcon::trigger, 10, below},
        {keyIdPower, {150,41,30,30}, "Power", DeviceControlIcon::power, 15, below},
        {keyIdStickPress, {28,50,106,106}, "Analogue stick", DeviceControlIcon::stick, 53, below},
        {keyIdHard1, {363,119,74,35}, "Bottom action button", action, 0, below, {344,48,112,112}},
        {keyIdHard2, {415,67,35,74}, "Right action button", action, 0, below, {344,48,112,112}},
        {keyIdHard3, {350,67,35,74}, "Left action button", action, 0, below, {344,48,112,112}},
        {keyIdHard4, {363,54,74,35}, "Top action button", action, 0, below, {344,48,112,112}},
        {keyIdFunction, {150,103,30,30}, "Function", DeviceControlIcon::function, 15, below},
        {keyIdHome, {150,149,30,30}, "Home", DeviceControlIcon::home, 15, below},
        {keyIdBluetooth, {228,40,24,24}, "Bluetooth", DeviceControlIcon::bluetooth, 12, below},
        {keyIdTriggerLeft, {8,8,120,24}, "Left trigger", DeviceControlIcon::trigger, 10, around},
        {keyIdPower, {94,40,30,30}, "Power", DeviceControlIcon::power, 15, around},
        {keyIdStickPress, {10,76,106,106}, "Analogue stick", DeviceControlIcon::stick, 53, around},
        {keyIdFunction, {94,204,30,30}, "Function", DeviceControlIcon::function, 15, around},
        {keyIdHome, {94,258,30,30}, "Home", DeviceControlIcon::home, 15, around},
        {keyIdTriggerRight, {172,8,120,24}, "Right trigger", DeviceControlIcon::trigger, 10, around},
        {keyIdHard1, {189,144,74,35}, "Bottom action button", action, 0, around, {170,73,112,112}},
        {keyIdHard2, {241,92,35,74}, "Right action button", action, 0, around, {170,73,112,112}},
        {keyIdHard3, {176,92,35,74}, "Left action button", action, 0, around, {170,73,112,112}},
        {keyIdHard4, {189,79,74,35}, "Top action button", action, 0, around, {170,73,112,112}},
        {keyIdBluetooth, {182,42,24,24}, "Bluetooth", DeviceControlIcon::bluetooth, 12, around},
    };
    static const DeviceControlProfile zodiacProfile{480,200,zodiac,std::size(zodiac),{0,0,0,0},0,{0,0,0,0},
                                                    DeviceControlStyle::zodiac,0.5f,1,0,0,{0,0,0,0},0,150,320};
    // Windows Mobile iPAQs. Their strips are authored 480 units wide: 2 units per pixel under a
    // 240-wide screen, 1 under the iPAQ 210's 480-wide one.
    // iPAQ 210: a thin black strip of four flat keys (Calendar, Start, OK, Messaging) split by a
    // chrome shield five-way, wider than tall, level with the strip's top and hanging about
    // 40% of its height below it, with a small recessed select key. The shield's art tapers to
    // big rounded lower corners; its hit area is the navigator rect with a 24-unit corner.
    static const DeviceButton ipaq210[] = {
        {keyIdHard1, {2,11,80,70}, "Calendar", DeviceControlIcon::calendarGrid},
        {keyIdHard2, {85,11,77,70}, "Start", DeviceControlIcon::start},
        {keyIdHard3, {318,11,77,70}, "OK", DeviceControlIcon::ok},
        {keyIdHard4, {398,11,80,70}, "Messaging", DeviceControlIcon::inbox},
        {keyIdUp, {222,16,36,34}, "Up", DeviceControlIcon::up},
        {keyIdDown, {222,98,36,31}, "Down", DeviceControlIcon::down},
        {keyIdLeft, {168,52,38,44}, "Left", DeviceControlIcon::left},
        {keyIdRight, {274,52,38,44}, "Right", DeviceControlIcon::right},
        {keyIdSelect, {209,51,62,42}, "Select", DeviceControlIcon::select},
    };
    static const DeviceControlProfile ipaq210Profile{480,150,ipaq210,std::size(ipaq210),{166,14,148,117},24,{209,51,62,42},
                                                     DeviceControlStyle::ipaq210,0.06f,1,6};
    // iPAQ rx1950 on a 240-wide screen: four round dark keys in silver rings (Calendar, Contacts,
    // Inbox, Wireless), the outer ones higher, close round a near-square mirror chrome five-way
    // with a dark glossy centre key. Power is on the top edge.
    static const DeviceButton rx1950[] = {
        {keyIdHard1, {9,51,66,66}, "Calendar", DeviceControlIcon::calendar},
        {keyIdHard2, {80,73,66,66}, "Contacts", DeviceControlIcon::contacts},
        {keyIdHard3, {334,73,66,66}, "Inbox", DeviceControlIcon::inbox},
        {keyIdHard4, {405,51,66,66}, "Wireless", DeviceControlIcon::wireless},
        {keyIdUp, {222,44,36,30}, "Up", DeviceControlIcon::up},
        {keyIdDown, {222,128,36,30}, "Down", DeviceControlIcon::down},
        {keyIdLeft, {174,84,36,34}, "Left", DeviceControlIcon::left},
        {keyIdRight, {270,84,36,34}, "Right", DeviceControlIcon::right},
        {keyIdSelect, {214,78,52,46}, "Select", DeviceControlIcon::select},
    };
    static const DeviceControlProfile rx1950Profile{480,178,rx1950,std::size(rx1950),{170,40,140,122},40,{214,78,52,46},
                                                    DeviceControlStyle::rx1950,0.5f,2,14};
    // iPAQ h4350 on a 240-wide screen, the body scaled to the strip's width: four round keys
    // (Calendar, Contacts, Inbox, Task switcher), the outer ones higher, round a chrome five-way
    // in a blue chrome band, then the thumb keyboard on a slate panel: ten columns on a 46-unit
    // pitch in four rows 44 units apart, each row sagging 24 units in the middle. Hiding the
    // keyboard leaves the row. The keyboard keys take placeholder ids from the keyboard range
    // until a board maps them.
    static const DeviceButton h4350[] = {
        {keyIdHard1, {48,42,44,44}, "Calendar", DeviceControlIcon::calendar},
        {keyIdHard2, {120,52,44,44}, "Contacts", DeviceControlIcon::contacts},
        {keyIdHard3, {316,52,44,44}, "Inbox", DeviceControlIcon::inbox},
        {keyIdHard4, {388,42,44,44}, "Task switcher", DeviceControlIcon::taskSwitch},
        {keyIdUp, {228,34,24,18}, "Up", DeviceControlIcon::up},
        {keyIdDown, {228,90,24,16}, "Down", DeviceControlIcon::down},
        {keyIdLeft, {192,58,22,24}, "Left", DeviceControlIcon::left},
        {keyIdRight, {266,58,22,24}, "Right", DeviceControlIcon::right},
        {keyIdSelect, {222,54,36,32}, "Select", DeviceControlIcon::select},
        {h4350Key(0), {17,117,32,30}, "Q", DeviceControlIcon::legend, 0, kb},
        {h4350Key(1), {63,126,32,30}, "W", DeviceControlIcon::legend, 0, kb},
        {h4350Key(2), {109,134,32,30}, "E", DeviceControlIcon::legend, 0, kb},
        {h4350Key(3), {155,138,32,30}, "R", DeviceControlIcon::legend, 0, kb},
        {h4350Key(4), {201,141,32,30}, "T", DeviceControlIcon::legend, 0, kb},
        {h4350Key(5), {247,141,32,30}, "Y", DeviceControlIcon::legend, 0, kb},
        {h4350Key(6), {293,138,32,30}, "U", DeviceControlIcon::legend, 0, kb},
        {h4350Key(7), {339,134,32,30}, "I", DeviceControlIcon::legend, 0, kb},
        {h4350Key(8), {385,126,32,30}, "O", DeviceControlIcon::legend, 0, kb},
        {h4350Key(9), {431,117,32,30}, "P", DeviceControlIcon::legend, 0, kb},
        {h4350Key(10), {17,161,32,30}, "A", DeviceControlIcon::legend, 0, kb},
        {h4350Key(11), {63,170,32,30}, "S", DeviceControlIcon::legend, 0, kb},
        {h4350Key(12), {109,178,32,30}, "D", DeviceControlIcon::legend, 0, kb},
        {h4350Key(13), {155,182,32,30}, "F", DeviceControlIcon::legend, 0, kb},
        {h4350Key(14), {201,185,32,30}, "G", DeviceControlIcon::legend, 0, kb},
        {h4350Key(15), {247,185,32,30}, "H", DeviceControlIcon::legend, 0, kb},
        {h4350Key(16), {293,182,32,30}, "J", DeviceControlIcon::legend, 0, kb},
        {h4350Key(17), {339,178,32,30}, "K", DeviceControlIcon::legend, 0, kb},
        {h4350Key(18), {385,170,32,30}, "L", DeviceControlIcon::legend, 0, kb},
        {h4350Key(19), {431,161,32,30}, "Backspace", DeviceControlIcon::legend, 0, kb},
        {h4350Key(20), {17,205,32,30}, "Shift", DeviceControlIcon::legend, 0, kb},
        {h4350Key(21), {63,214,32,30}, "Z", DeviceControlIcon::legend, 0, kb},
        {h4350Key(22), {109,222,32,30}, "X", DeviceControlIcon::legend, 0, kb},
        {h4350Key(23), {155,226,32,30}, "C", DeviceControlIcon::legend, 0, kb},
        {h4350Key(24), {201,229,32,30}, "V", DeviceControlIcon::legend, 0, kb},
        {h4350Key(25), {247,229,32,30}, "B", DeviceControlIcon::legend, 0, kb},
        {h4350Key(26), {293,226,32,30}, "N", DeviceControlIcon::legend, 0, kb},
        {h4350Key(27), {339,222,32,30}, "M", DeviceControlIcon::legend, 0, kb},
        {h4350Key(28), {385,214,32,30}, "Exclamation mark", DeviceControlIcon::legend, 0, kb},
        {h4350Key(29), {431,205,32,30}, "OK", DeviceControlIcon::legend, 0, kb},
        {h4350Key(30), {17,249,32,30}, "Fn", DeviceControlIcon::legend, 0, kb},
        {h4350Key(31), {63,258,32,30}, "Tab", DeviceControlIcon::legend, 0, kb},
        {h4350Key(32), {109,266,32,30}, "Start", DeviceControlIcon::legend, 0, kb},
        {h4350Key(33), {155,270,32,30}, "Today", DeviceControlIcon::legend, 0, kb},
        {h4350Key(34), {200,273,80,30}, "Space", DeviceControlIcon::legend, 0, kb},
        {h4350Key(35), {293,270,32,30}, "Full stop", DeviceControlIcon::legend, 0, kb},
        {h4350Key(36), {339,266,32,30}, "Comma", DeviceControlIcon::legend, 0, kb},
        {h4350Key(37), {385,258,32,30}, "Question mark", DeviceControlIcon::legend, 0, kb},
        {h4350Key(38), {431,249,32,30}, "Apostrophe", DeviceControlIcon::legend, 0, kb},
    };
    static const DeviceControlProfile h4350Profile{480,324,h4350,std::size(h4350),{190,32,100,76},26,{222,54,36,32},
                                                   DeviceControlStyle::h4350,0.5f,2,10,208};
    // CLIE PEG-T415 under its 320-wide screen, 1 unit per pixel: four wide, flat chrome slats
    // edge to edge (Date Book, Address, To Do, Memo Pad) either side of a narrow rocker split
    // across its middle, under a dark metal strip with the keys' icons stamped on it. The Jog
    // Dial and Back are on the left side, beside the top half of the LCD, as on the TG50. Power
    // is not on this face.
    static const DeviceButton t415[] = {
        {keyIdHard1, {4,40,66,38}, "Date Book", DeviceControlIcon::calendar, 3},
        {keyIdHard2, {72,40,66,38}, "Address", DeviceControlIcon::phone, 3},
        {keyIdHard3, {182,40,66,38}, "To Do", DeviceControlIcon::tasks, 3},
        {keyIdHard4, {250,40,66,38}, "Memo Pad", DeviceControlIcon::memo, 3},
        {keyIdUp, {143,40,34,19}, "Up", DeviceControlIcon::up, 3, DeviceControlSection::main, {143,40,34,38}},
        {keyIdDown, {143,59,34,19}, "Down", DeviceControlIcon::down, 3, DeviceControlSection::main, {143,40,34,38}},
        {keyIdJogUp, {-34,-418,30,26}, "Jog up", DeviceControlIcon::up, 6, tab},
        {keyIdJogPress, {-34,-386,30,64}, "Jog press", DeviceControlIcon::select, 6, tab},
        {keyIdJogDown, {-34,-316,30,26}, "Jog down", DeviceControlIcon::down, 6, tab},
        {keyIdBack, {-34,-274,30,26}, "Back", DeviceControlIcon::back, 6, tab},
    };
    static const DeviceControlProfile t415Profile{320,104,t415,std::size(t415),{0,0,0,0},0,{0,0,0,0},
                                                  DeviceControlStyle::t415,0.5f,1,0,0,{-38,-424,38,208},38,0,0,128};
    static const DeviceControlProfile empty{0,0,nullptr,0,{0,0,0,0},0,{0,0,0,0}};
    switch(art) {
        case DeviceArt::tx: return txProfile;
        case DeviceArt::e2: return e2Profile;
        case DeviceArt::zire72: return zire72Profile;
        case DeviceArt::m515: return m515Profile;
        case DeviceArt::iiix: return iiixProfile;
        case DeviceArt::m125: return m125Profile;
        case DeviceArt::iiie: return iiieProfile;
        case DeviceArt::iiixe: return iiixeProfile;
        case DeviceArt::m100: return m100Profile;
        case DeviceArt::m105: return m105Profile;
        case DeviceArt::v: return vProfile;
        case DeviceArt::vx: return vxProfile;
        case DeviceArt::m500: return m500Profile;
        case DeviceArt::c500: return c500Profile;
        case DeviceArt::m505: return m505Profile;
        case DeviceArt::m520: return m520Profile;
        case DeviceArt::m130: return m130Profile;
        case DeviceArt::iiic: return iiicProfile;
        case DeviceArt::tg50: return tg50Profile;
        case DeviceArt::t415: return t415Profile;
        case DeviceArt::zodiac: return zodiacProfile;
        case DeviceArt::hpIpaq210: return ipaq210Profile;
        case DeviceArt::hpIpaqRx1950: return rx1950Profile;
        case DeviceArt::hpIpaqH4350: return h4350Profile;
        default: return empty;
    }
}
inline const DeviceControlProfile& deviceControls(DeviceType5 device) {return deviceControls(deviceArt(device));}
inline bool deviceControlShown(const DeviceButton& button, const DeviceControlView& view) {
    switch(button.section) {
        case DeviceControlSection::main: return true;
        case DeviceControlSection::keyboard: return view.keyboard;
        case DeviceControlSection::sideTab: return view.sideTab;
        case DeviceControlSection::below: return view.arrangement==DeviceControlArrangement::below;
        case DeviceControlSection::around: return view.arrangement==DeviceControlArrangement::around;
    }
    return false;
}
// True when the controls sit in panels either side of the screen and nothing goes under it.
inline bool deviceControlsAround(const DeviceControlProfile& profile, const DeviceControlView& view) {
    return profile.panelWidth>0 && view.arrangement==DeviceControlArrangement::around;
}
// Visible lower controls keep their authored aspect ratio across the full TG50 body.
inline int deviceControlsUnits(const DeviceControlProfile& profile, const DeviceControlView& view) {
    if(deviceControlsAround(profile,view))return 0;
    return view.keyboard ? profile.height : profile.height-profile.keyboardHeight;
}
inline bool deviceSideTabBelowScreen(const DeviceControlProfile& profile, const DeviceControlView& view) {
    return profile.sideTabWidth>0 && profile.keyboardHeight>0 && view.sideTab && view.keyboard && view.sideTabBesideKeyboard;
}
// A device without a keyboard: the side tab left of the silkscreen and strip, under a full-width LCD.
inline bool deviceSideTabBesideButtons(const DeviceControlProfile& profile, const DeviceControlView& view) {
    return profile.sideTabWidth>0 && profile.keyboardHeight==0 && profile.silkscreenHeight>0 && view.sideTab &&
           view.sideTabBesideKeyboard;
}
inline int deviceScreenLeft(const DeviceControlProfile& profile, const DeviceControlView& view={}) {
    if(deviceControlsAround(profile,view))return profile.panelWidth/profile.unit;
    return view.sideTab && !deviceSideTabBelowScreen(profile,view) && !deviceSideTabBesideButtons(profile,view)
               ? profile.sideTabWidth/profile.unit : 0;
}
inline int deviceControlsWidth(const DeviceControlProfile& profile, int screenWidth, const DeviceControlView& view={}) {
    return screenWidth+deviceScreenLeft(profile,view)+
           (deviceControlsAround(profile,view) ? profile.panelWidth/profile.unit : 0);
}
// Top and bottom of the side tab's keys, in side tab units.
inline std::pair<int,int> deviceSideTabKeys(const DeviceControlProfile& profile) {
    int top=0,bottom=0;
    bool any=false;
    for(size_t i=0;i<profile.count;++i) {
        const auto& r=profile.buttons[i].bounds;
        if(profile.buttons[i].section!=DeviceControlSection::sideTab)continue;
        top=any ? std::min(top,r.y) : r.y;bottom=any ? std::max(bottom,r.y+r.h) : r.y+r.h;any=true;
    }
    return {top,bottom};
}
// Strip units to window pixels: x=left+sx*scale, y=screenHeight+top+sy*scale; the Zodiac's strip is
// scaled to the screen's width so it fits at 90 and 270 too. Side tab units map
// 1:unit from (sidebarLeft, screenHeight+sidebarTop). screenHeight is the LCD's height plus the
// silkscreen's at full size.
struct DeviceControlTransform {
    const float scale;
    const float left;
    // Window pixels from screenHeight to the strip's top: negative beside the buttons, where the
    // silkscreen above the strip is drawn at its scale, rounded to whole rows.
    const int top;
    const float sidebarLeft;
    const int sidebarTop;
    DeviceControlTransform(const DeviceControlProfile& profile, int screenWidth, const DeviceControlView& view)
        : scale(profile.style==DeviceControlStyle::zodiac ? float(screenWidth)/profile.width
                : deviceSideTabBelowScreen(profile,view) || deviceSideTabBesideButtons(profile,view)
                    ? float(screenWidth-profile.sideTabWidth/profile.unit)/profile.width
                : profile.style==DeviceControlStyle::tg50 ? float(deviceControlsWidth(profile,screenWidth,view))/profile.width
                : 1.f/profile.unit),
          left(deviceSideTabBelowScreen(profile,view) || deviceSideTabBesideButtons(profile,view)
                    ? float(profile.sideTabWidth/profile.unit)
                : profile.style==DeviceControlStyle::tg50 ? 0.f
                : profile.style==DeviceControlStyle::zodiac ? float(deviceScreenLeft(profile,view))
                : deviceScreenLeft(profile,view)+float((screenWidth*profile.unit-profile.width)/2)/profile.unit),
          top(deviceSideTabBesideButtons(profile,view)
                    ? int(std::lround(profile.silkscreenHeight*scale))-profile.silkscreenHeight/profile.unit : 0),
          sidebarLeft(deviceSideTabBelowScreen(profile,view) || deviceSideTabBesideButtons(profile,view)
                    ? float(profile.sideTabWidth/profile.unit)
                    : deviceScreenLeft(profile,view)+(profile.style==DeviceControlStyle::tg50 ? 0
                    : (screenWidth*profile.unit-profile.width)/2/profile.unit)),
          // Below the LCD the tab's keys are centred on a whole pixel: on the strip's height left of
          // the keyboard, on the silkscreen's and strip's together beside the buttons.
          sidebarTop(deviceSideTabBelowScreen(profile,view) || deviceSideTabBesideButtons(profile,view)
                    ? int(std::lround((deviceControlsUnits(profile,view)*scale+top-profile.silkscreenHeight/profile.unit*
                                           float(deviceSideTabBesideButtons(profile,view))-
                                       float(deviceSideTabKeys(profile).second+deviceSideTabKeys(profile).first)/profile.unit)*0.5f))
                    : 0) {}
};
// Window rows the controls add below screenHeight, less beside the buttons, where the silkscreen
// shrinks. Rounded up once in device pixels; drawing and input share the unrounded uniform scale.
inline int deviceControlsHeight(const DeviceControlProfile& profile, int screenWidth, const DeviceControlView& view={}) {
    const DeviceControlTransform transform(profile,screenWidth,view);
    return transform.top+int(std::ceil(deviceControlsUnits(profile,view)*transform.scale));
}
inline int deviceControlsHeight(const DeviceControlProfile& profile, const DeviceControlView& view={}) {
    return deviceControlsHeight(profile,profile.width/profile.unit,view);
}
// Signed distance from (x,y) to a rounded rectangle; negative inside.
inline float deviceRoundedDistance(const SDL_Rect& r, float radius, float x, float y) {
    const float qx=std::fabs(x-r.x-r.w*0.5f)-r.w*0.5f+radius;
    const float qy=std::fabs(y-r.y-r.h*0.5f)-r.h*0.5f+radius;
    return std::hypot(std::max(qx,0.f),std::max(qy,0.f))+std::min(std::max(qx,qy),0.f)-radius;
}
// Five-way part at a strip point: the select pill, else the ring split along its diagonals.
inline KeyId deviceNavigatorAt(const DeviceControlProfile& profile, float x, float y) {
    const auto& n=profile.navigator;
    if(n.w<=0 || deviceRoundedDistance(n,profile.navigatorCorner,x,y)>0)return keyInvalid;
    const auto& s=profile.select;
    if(deviceRoundedDistance(s,profile.selectCorner>0 ? profile.selectCorner : s.h*0.5f,x,y)<=1)return keyIdSelect;
    const float dx=x-n.x-n.w*0.5f,dy=y-n.y-n.h*0.5f;
    if(std::fabs(dx)*n.h>std::fabs(dy)*n.w)return dx<0 ? keyIdLeft : keyIdRight;
    return dy<0 ? keyIdUp : keyIdDown;
}
// How far below its authored place an around key sits on a panel taller than authored, in strip
// units: the triggers stay at the top, the rest are centred.
inline int deviceAroundTop(const DeviceControlProfile& profile, const DeviceButton& button, int screenHeight) {
    if(button.icon==DeviceControlIcon::trigger)return 0;
    return std::max(0,screenHeight*profile.unit-profile.panelHeight)/2;
}
// Window offset of an around key from its authored place: right panel keys move past the screen.
inline SDL_Point deviceAroundOffset(const DeviceControlProfile& profile, const DeviceButton& button,
                                   int screenWidth, int screenHeight) {
    return {button.bounds.x>=profile.panelWidth ? screenWidth : 0,
            deviceAroundTop(profile,button,screenHeight)/profile.unit};
}
inline SDL_Rect deviceButtonRect(const DeviceControlProfile& profile,
                                const DeviceButton& button, int screenWidth, int screenHeight,
                                const DeviceControlView& view={}) {
    if(button.section==DeviceControlSection::around) {
        const auto offset=deviceAroundOffset(profile,button,screenWidth,screenHeight);
        auto r=button.bounds;
        r.x=r.x/profile.unit+offset.x;r.y=r.y/profile.unit+offset.y;r.w/=profile.unit;r.h/=profile.unit;
        return r;
    }
    if(profile.style!=DeviceControlStyle::tg50 && profile.style!=DeviceControlStyle::zodiac &&
       !deviceSideTabBesideButtons(profile,view)) {
        const int u=profile.unit;
        auto r=button.bounds;
        r.x=(r.x+(screenWidth*u-profile.width)/2)/u+deviceScreenLeft(profile,view);
        r.y=r.y/u+screenHeight;r.w/=u;r.h/=u;
        return r;
    }
    const DeviceControlTransform transform(profile,screenWidth,view);
    const bool sidebar=button.section==DeviceControlSection::sideTab;
    const float scale=sidebar ? 1.f/profile.unit : transform.scale;
    const float left=sidebar ? transform.sidebarLeft : transform.left;
    const int top=screenHeight+(sidebar ? transform.sidebarTop : transform.top);
    const auto& r=button.bounds;
    const int x=int(std::lround(left+r.x*scale)),y=int(std::lround(top+r.y*scale));
    return {x,y,int(std::lround(left+(r.x+r.w)*scale))-x,
                int(std::lround(top+(r.y+r.h)*scale))-y};
}
// App keys are rounded rectangles; their pockets share this corner.
inline float deviceButtonCorner(const DeviceControlProfile& profile, const SDL_Rect& r) {
    return std::min(r.w,r.h)*profile.buttonCorner;
}
inline float deviceButtonCorner(const DeviceControlProfile& profile, const DeviceButton& button) {
    return button.corner>0 ? button.corner : deviceButtonCorner(profile,button.bounds);
}
inline bool deviceNavigatorButton(const DeviceControlProfile& profile, const DeviceButton& button) {
    return profile.navigator.w>0 && button.icon>=DeviceControlIcon::up &&
           button.icon<=DeviceControlIcon::select;
}
// A key of a round cluster split along its diagonals, like the Zodiac's Action keys: the button's
// shape is the cluster's recess and the key takes the quarter its bounds lie in, outside a small
// dead middle. Strip units.
constexpr float deviceClusterMiddle=0.12f;
inline bool deviceClusterKeyAt(const DeviceButton& b, float x, float y) {
    const auto& s=b.shape;
    const float cx=s.x+s.w*0.5f,cy=s.y+s.h*0.5f,dx=x-cx,dy=y-cy,d=std::hypot(dx,dy);
    if(d>s.w*0.5f || d<s.w*deviceClusterMiddle)return false;
    const float kx=b.bounds.x+b.bounds.w*0.5f-cx,ky=b.bounds.y+b.bounds.h*0.5f-cy;
    if(std::fabs(kx)>std::fabs(ky))return std::fabs(dx)>=std::fabs(dy) && dx*kx>0;
    return std::fabs(dy)>std::fabs(dx) && dy*ky>0;
}
inline KeyId deviceControlAt(const DeviceControlProfile& profile, int screenWidth,
                             int screenHeight, int x, int y, const DeviceControlView& view={}) {
    // Integer window points hit-test at their pixel centre, in strip units.
    const DeviceControlTransform transform(profile,screenWidth,view);
    const float sx=(x+0.5f-transform.left)/transform.scale,sy=(y-screenHeight-transform.top+0.5f)/transform.scale;
    const auto part=deviceNavigatorAt(profile,sx,sy);
    if(part!=keyInvalid)return part;
    for(size_t i=0;i<profile.count;++i) {
        if(deviceNavigatorButton(profile,profile.buttons[i]) || !deviceControlShown(profile.buttons[i],view))continue;
        const auto& b=profile.buttons[i];
        const auto& r=b.bounds;
        if(b.icon==DeviceControlIcon::stick)continue;
        const bool sidebar=b.section==DeviceControlSection::sideTab;
        const bool panel=b.section==DeviceControlSection::around;
        const SDL_Point offset=panel ? deviceAroundOffset(profile,b,screenWidth,screenHeight) : SDL_Point{0,0};
        const float bx=panel ? (x+0.5f-offset.x)*profile.unit : sidebar ? (x+0.5f-transform.sidebarLeft)*profile.unit : sx;
        const float by=panel ? (y+0.5f-offset.y)*profile.unit : sidebar ? (y-screenHeight-transform.sidebarTop+0.5f)*profile.unit : sy;
        if(b.icon==DeviceControlIcon::action && b.shape.w>0) {
            if(deviceClusterKeyAt(b,bx,by))return b.key;
        } else if(b.shape.w>0) {
            if(bx>=r.x && bx<r.x+r.w && by>=r.y && by<r.y+r.h &&
               deviceRoundedDistance(b.shape,deviceButtonCorner(profile,b),bx,by)<=0)
                return b.key;
        } else if(deviceRoundedDistance(r,deviceButtonCorner(profile,b),bx,by)<=0)
            return b.key;
    }
    return keyInvalid;
}
// The analogue stick's well in window pixels for this view, w 0 for none.
inline SDL_Rect deviceStickRect(const DeviceControlProfile& profile, int screenWidth, int screenHeight,
                                const DeviceControlView& view={}) {
    for(size_t i=0;i<profile.count;++i) {
        const auto& b=profile.buttons[i];
        if(b.icon==DeviceControlIcon::stick && deviceControlShown(b,view))
            return deviceButtonRect(profile,b,screenWidth,screenHeight,view);
    }
    return {0,0,0,0};
}
// The stick's knob travels this fraction of the well's width from its centre either way; its
// radius is deviceStickKnob of the well's width.
constexpr float deviceStickTravel=0.25f;
constexpr float deviceStickKnob=0.27f;
// True when a window point (device pixels) is on the stick's round well.
inline bool deviceStickGrab(const SDL_Rect& well, float x, float y) {
    if(well.w<=0)return false;
    return std::hypot(x-well.x-well.w*0.5f,y-well.y-well.h*0.5f)<=well.w*0.5f;
}
// True when a window point is on the knob at rest, where a tap pushes the stick in.
inline bool deviceStickKnobAt(const SDL_Rect& well, float x, float y) {
    if(well.w<=0)return false;
    return std::hypot(x-well.x-well.w*0.5f,y-well.y-well.h*0.5f)<=well.w*deviceStickKnob;
}
// True once a point held on the knob has moved far enough to be a drag rather than a press.
inline bool deviceStickDragged(const SDL_Rect& well, float x0, float y0, float x, float y) {
    return std::hypot(x-x0,y-y0)>std::max(2.f,well.w*0.08f);
}
// The stick held at a window point: its offset from the well's centre over the knob's travel,
// held to the unit circle, so a finger can leave the well and keep full deflection.
inline DeviceStick deviceStickAt(const SDL_Rect& well, float x, float y) {
    const float travel=std::max(1.f,well.w*deviceStickTravel);
    float dx=(x-well.x-well.w*0.5f)/travel,dy=(y-well.y-well.h*0.5f)/travel;
    const float length=std::hypot(dx,dy);
    if(length>1) {dx/=length;dy/=length;}
    return {dx,dy,true};
}
// Around in a landscape or square window, or with no window, below in a portrait one.
inline DeviceControlView deviceControlViewForWindow(const DeviceControlProfile& profile, DeviceControlView view,
                                                    int windowWidth, int windowHeight) {
    if(profile.panelWidth<=0)return view;
    view.arrangement=windowWidth>0 && windowHeight>windowWidth ? DeviceControlArrangement::below
                                                               : DeviceControlArrangement::around;
    return view;
}
void drawDeviceControls(SDL_Renderer* renderer, const DeviceControlProfile& profile,
                        int screenWidth, int screenHeight, float scale, DeviceKeyMask pressed,
                        const DeviceControlView& view={}, DeviceStick stick={});

// Panels beside the screen: the TG50's side tab, or the Zodiac's two panels when around.
void drawDeviceSidebar(SDL_Renderer* renderer, const DeviceControlProfile& profile,
                       int screenWidth, int screenHeight, float scale, DeviceKeyMask pressed,
                       const DeviceControlView& view={}, DeviceStick stick={});

// AlphaSmart Dana keyboard art, drawn below the 560x160 LCD at its width. Keys carry the
// physical names of dana_keyboard5.h; they have no KeyId and no bit in DeviceKeyMask.
// Geometry is in keyboard units: 600 wide, main keys on a 40 unit pitch, top row and arrow
// row on 34. Bounds are the whole cell a key hits in; the cap is drawn 2 units inside it.
enum class DanaKeyMark { none, tab, backspace, enter, up, down, left, right, command, apps };
struct DanaKeyArt {
    const char* name;
    SDL_Rect bounds;
    // White legends: the main one, and the shifted one printed above it.
    const char* legend;
    const char* upper;
    // Yellow function-layer legend.
    const char* second;
    DanaKeyMark mark;
    // Lighter cap colour of the top row.
    bool topRow;
};
struct DanaKeyboardArt {
    int width, height;
    const DanaKeyArt* keys;
    size_t count;
    // Dark well round the keys: the main block, and the arrow block below its right end.
    SDL_Rect well, arrowWell;
};
const DanaKeyboardArt& danaKeyboardArt();
// Art index of a physical key name, or -1.
inline int danaKeyArtIndex(std::string_view name) {
    const auto& art=danaKeyboardArt();
    for(size_t i=0;i<art.count;++i) if(name==art.keys[i].name) return int(i);
    return -1;
}
// Key under a point in keyboard units, or -1 in the gaps and on the body.
inline int danaKeyAt(float x, float y) {
    const auto& art=danaKeyboardArt();
    for(size_t i=0;i<art.count;++i) {
        const auto& r=art.keys[i].bounds;
        if(x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h) return int(i);
    }
    return -1;
}
// Held keys by art index, any number at once.
class DanaKeysHeld {
    std::bitset<128> held;
  public:
    bool Set(std::string_view name, bool down) {
        const int index=danaKeyArtIndex(name);
        if(index<0) return false;
        held.set(size_t(index),down);
        return true;
    }
    bool Held(size_t index) const { return index<held.size() && held.test(index); }
    size_t Count() const { return held.count(); }
    void Clear() { held.reset(); }
};
// Keyboard height in window pixels when drawn width pixels wide.
inline int danaKeyboardHeight(int width) {
    const auto& art=danaKeyboardArt();
    return int(std::lround(float(width)*art.height/art.width));
}
// Draws the keyboard into dest, scaled to its width; dest.h should be danaKeyboardHeight(dest.w).
void drawDanaKeyboard(SDL_Renderer* renderer, const SDL_Rect& dest, const DanaKeysHeld& held);
// The same cap and legend drawing with independently supplied compact bounds.
void drawDanaKeyCaps(SDL_Renderer* renderer,const SDL_Rect& dest,int width,int height,
                     const DanaKeyArt* keys,size_t count,const std::vector<bool>& held);

// The LCD with the keyboard below it, in one column as large as fits a view and centred.
// Portrait and landscape use the same column; the guest picture is never rotated. Without
// the keyboard the LCD fills the column alone.
struct DanaLayout { SDL_Rect screen, keyboard; };
inline DanaLayout danaLayout(int viewWidth, int viewHeight, bool keyboard) {
    const auto& art=danaKeyboardArt();
    const float perWidth=160.f/560+(keyboard ? float(art.height)/art.width : 0.f);
    const int width=std::max(0,std::min(viewWidth,int(std::floor(viewHeight/perWidth))));
    const int lcd=int(std::lround(width*160.f/560));
    const int below=keyboard ? danaKeyboardHeight(width) : 0;
    const int x=(viewWidth-width)/2,y=(viewHeight-lcd-below)/2;
    return {{x,y,width,lcd},{x,y+lcd,keyboard ? width : 0,below}};
}

// Printed silkscreen under the LCD. Taps on it reach the guest as pen input; these
// rectangles only place the artwork.
enum class SilkscreenIcon { home, menu, favorites, find, clock, brightness, letters, numbers, calc, contrast };
// E2 and Zire 72: two Graffiti areas between full-height lines; the 68k Palms: one rounded box.
enum class SilkscreenStyle { e2, zire72, m515, iiix, m125, v, iiic, t415, m500 };
struct SilkscreenButton {
    SDL_Rect bounds;
    const char* label;
    SilkscreenIcon icon;
};
struct DeviceSilkscreen {
    int width, height;
    const SilkscreenButton* buttons;
    size_t count;
    // Graffiti letter and number areas.
    SDL_Rect letters, numbers;
    SilkscreenStyle style{SilkscreenStyle::e2};
    // Silkscreen units per LCD pixel, as for the strip.
    int unit{1};
};
inline const DeviceSilkscreen& deviceSilkscreen(DeviceArt art) {
    // The E2 ROM's 'aslk' 19000 rectangles, in LCD pixels (twice the Palm OS
    // coordinates) from the top of the silkscreen. Menu, Find, abc and 123 run
    // past the 120 rows the digitizer reaches, as in the ROM.
    static const SilkscreenButton e2[] = {
        {{0,8,46,56}, "Home", SilkscreenIcon::home},
        {{0,68,46,56}, "Menu", SilkscreenIcon::menu},
        {{274,8,46,56}, "Favorites", SilkscreenIcon::favorites},
        {{274,68,46,56}, "Find", SilkscreenIcon::find},
        {{50,8,22,38}, "Clock", SilkscreenIcon::clock},
        {{250,8,22,38}, "Brightness", SilkscreenIcon::brightness},
        {{50,100,36,40}, "Letter keyboard", SilkscreenIcon::letters},
        {{236,100,36,40}, "Number keyboard", SilkscreenIcon::numbers},
    };
    static const DeviceSilkscreen e2Silkscreen{320,120,e2,std::size(e2),{54,4,124,112},{178,4,88,112}};
    // Zire 72: the NOR's 'aslk' 19000 (at 0x7c340), doubled from row 320 as for the E2.
    // Favorites and Find run to x 330 in the ROM; kept to the panel's width here.
    static const SilkscreenButton zire72[] = {
        {{0,4,46,56}, "Home", SilkscreenIcon::home},
        {{0,64,46,56}, "Menu", SilkscreenIcon::menu},
        {{276,8,44,56}, "Favorites", SilkscreenIcon::favorites},
        {{276,64,44,56}, "Find", SilkscreenIcon::find},
        {{46,4,22,38}, "Clock", SilkscreenIcon::clock},
        {{242,4,32,38}, "Brightness", SilkscreenIcon::brightness},
        {{46,92,36,40}, "Letter keyboard", SilkscreenIcon::letters},
        {{230,92,46,40}, "Number keyboard", SilkscreenIcon::numbers},
    };
    static const DeviceSilkscreen zire72Silkscreen{320,120,zire72,std::size(zire72),{54,4,124,132},{178,4,88,132},
                                                   SilkscreenStyle::zire72};
    // m515: 'silk' 10002 of the ROM's device resources (clock and brightness), in units of half
    // an LCD pixel from row 160, so Palm OS coordinates doubled as above. Home and Menu start at
    // x -6 and Calc and Find run to 324 in the ROM; kept to the panel's width here.
    static const SilkscreenButton m515[] = {
        {{0,6,42,54}, "Home", SilkscreenIcon::home},
        {{0,62,42,56}, "Menu", SilkscreenIcon::menu},
        {{280,6,40,60}, "Calc", SilkscreenIcon::calc},
        {{280,62,40,76}, "Find", SilkscreenIcon::find},
        {{44,6,22,38}, "Clock", SilkscreenIcon::clock},
        {{254,6,22,38}, "Brightness", SilkscreenIcon::brightness},
        {{44,92,28,46}, "Letter keyboard", SilkscreenIcon::letters},
        {{248,88,30,50}, "Number keyboard", SilkscreenIcon::numbers},
    };
    static const DeviceSilkscreen m515Silkscreen{320,120,m515,std::size(m515),{54,8,124,112},{178,8,88,112},
                                                 SilkscreenStyle::m515,2};
    // IIIx: System 'silk' 10000 of the ROM, doubled from row 160 as for the m515.
    static const SilkscreenButton iiix[] = {
        {{0,8,54,56}, "Home", SilkscreenIcon::home},
        {{0,64,54,56}, "Menu", SilkscreenIcon::menu},
        {{266,8,54,56}, "Calc", SilkscreenIcon::calc},
        {{266,64,54,56}, "Find", SilkscreenIcon::find},
        {{54,92,36,28}, "Letter keyboard", SilkscreenIcon::letters},
        {{230,92,36,28}, "Number keyboard", SilkscreenIcon::numbers},
    };
    static const DeviceSilkscreen iiixSilkscreen{320,120,iiix,std::size(iiix),{54,8,124,112},{178,8,88,112},
                                                 SilkscreenStyle::iiix,2};
    // m125: 'silk' 10000 of the ROM's DeviceResources (clock and contrast), doubled from row 160 as
    // for the m515. Home and Menu start at x -6 in the ROM; kept to the panel's width here.
    static const SilkscreenButton m125[] = {
        {{0,6,42,54}, "Home", SilkscreenIcon::home},
        {{0,62,42,56}, "Menu", SilkscreenIcon::menu},
        {{274,6,44,60}, "Calc", SilkscreenIcon::calc},
        {{274,62,44,76}, "Find", SilkscreenIcon::find},
        {{44,6,22,38}, "Clock", SilkscreenIcon::clock},
        {{254,6,22,38}, "Contrast", SilkscreenIcon::contrast},
        {{44,92,28,46}, "Letter keyboard", SilkscreenIcon::letters},
        {{242,88,30,50}, "Number keyboard", SilkscreenIcon::numbers},
    };
    static const DeviceSilkscreen m125Silkscreen{320,120,m125,std::size(m125),{54,8,124,112},{178,8,88,112},
                                                 SilkscreenStyle::m125,2};
    // V and Vx: the IIIx's table, the same in the Vx 4.1 ROM; printed like the m125's.
    static const DeviceSilkscreen vSilkscreen{320,120,iiix,std::size(iiix),{54,8,124,112},{178,8,88,112},
                                              SilkscreenStyle::v,2};
    // m500 and WorkPad c500: 'silk' 10000 of the ROM's DeviceResources (clock and contrast), the
    // m515's rectangles with contrast in place of brightness, printed light with dark icons. The
    // c500 ROM's table is the same.
    static const SilkscreenButton m500[] = {
        {{0,6,42,54}, "Home", SilkscreenIcon::home},
        {{0,62,42,56}, "Menu", SilkscreenIcon::menu},
        {{280,6,40,60}, "Calc", SilkscreenIcon::calc},
        {{280,62,40,76}, "Find", SilkscreenIcon::find},
        {{44,6,22,38}, "Clock", SilkscreenIcon::clock},
        {{254,6,22,38}, "Contrast", SilkscreenIcon::contrast},
        {{44,92,28,46}, "Letter keyboard", SilkscreenIcon::letters},
        {{248,88,30,50}, "Number keyboard", SilkscreenIcon::numbers},
    };
    static const DeviceSilkscreen m500Silkscreen{320,120,m500,std::size(m500),{54,8,124,112},{178,8,88,112},
                                                 SilkscreenStyle::m500,2};
    // m505 and m520: 'silk' 10000 of the ROM's DeviceResources, the m125's rectangles with clock
    // and contrast, printed like the m515's.
    static const DeviceSilkscreen m505Silkscreen{320,120,m125,std::size(m125),{54,8,124,112},{178,8,88,112},
                                                 SilkscreenStyle::m515,2};
    // IIIc: the IIIx's table, the same in the IIIc ROM, printed light on dark.
    static const DeviceSilkscreen iiicSilkscreen{320,120,iiix,std::size(iiix),{54,8,124,112},{178,8,88,112},
                                                 SilkscreenStyle::iiic,2};
    // T415: the ROM's SilkScreenVenice 'silk' 10000 (19000 is the same), doubled from row 160 into
    // LCD pixels under the 320x320 screen. Calc and Find run to x 326 in the ROM; kept to the
    // panel's width here. The contrast spot is key 0x123 at the bottom left; the panel ends at
    // Palm OS row 224, just below the lowest rectangle.
    static const SilkscreenButton t415[] = {
        {{0,16,34,40}, "Home", SilkscreenIcon::home},
        {{0,66,34,38}, "Menu", SilkscreenIcon::menu},
        {{286,16,34,40}, "Calc", SilkscreenIcon::calc},
        {{286,72,34,40}, "Find", SilkscreenIcon::find},
        {{54,106,22,18}, "Letter keyboard", SilkscreenIcon::letters},
        {{252,108,22,18}, "Number keyboard", SilkscreenIcon::numbers},
        {{0,110,4,16}, "Contrast", SilkscreenIcon::contrast},
    };
    static const DeviceSilkscreen t415Silkscreen{320,128,t415,std::size(t415),{52,16,122,106},{174,16,94,108},
                                                 SilkscreenStyle::t415,1};
    static const DeviceSilkscreen none{0,0,nullptr,0,{0,0,0,0},{0,0,0,0}};
    switch(art) {
        case DeviceArt::e2: return e2Silkscreen;
        case DeviceArt::zire72: return zire72Silkscreen;
        case DeviceArt::m515: return m515Silkscreen;
        case DeviceArt::iiix: case DeviceArt::iiie: case DeviceArt::iiixe: return iiixSilkscreen;
        // The m100, m105 and m130 ROMs carry the m125's rectangles, with clock and contrast.
        case DeviceArt::m125: case DeviceArt::m100: case DeviceArt::m105: case DeviceArt::m130: return m125Silkscreen;
        case DeviceArt::m505: case DeviceArt::m520: return m505Silkscreen;
        case DeviceArt::iiic: return iiicSilkscreen;
        case DeviceArt::v: case DeviceArt::vx: return vSilkscreen;
        case DeviceArt::m500: case DeviceArt::c500: return m500Silkscreen;
        case DeviceArt::t415: return t415Silkscreen;
        default: return none;
    }
}
inline const DeviceSilkscreen& deviceSilkscreen(DeviceType5 device) {
    const auto* profile = deviceProfile5(device);
    return deviceSilkscreen(profile && profile->frontend.width > 0 && !profile->frontend.silkscreenHeight
                                ? DeviceArt::none : deviceArt(device));
}
// Draws the silkscreen at scale (device pixels per LCD pixel) into a new texture the caller owns.
SDL_Texture* createDeviceSilkscreen(SDL_Renderer* renderer, const DeviceSilkscreen& silkscreen, float scale);
// Where the silkscreen goes in the window, screenHeight being the LCD's height plus its own as for
// the strip: under the LCD at its width, or beside the side tab at the strip's scale, as many rows
// as the strip's top moved up.
inline SDL_Rect deviceSilkscreenRect(const DeviceControlProfile& profile, const DeviceSilkscreen& silkscreen,
                                     int screenWidth, int screenHeight, const DeviceControlView& view={}) {
    const int height=silkscreen.height/silkscreen.unit;
    if(!deviceSideTabBesideButtons(profile,view))return {deviceScreenLeft(profile,view),screenHeight-height,screenWidth,height};
    const DeviceControlTransform transform(profile,screenWidth,view);
    const int left=int(transform.left);
    return {left,screenHeight-height,screenWidth-left,height+transform.top};
}
// A window point on the silkscreen drawn in rect, in LCD pixels from the silkscreen's top left:
// what the digitizer reads there.
inline SDL_Point deviceSilkscreenPoint(const SDL_Rect& rect, const DeviceSilkscreen& silkscreen, int x, int y) {
    const int width=silkscreen.width/silkscreen.unit,height=silkscreen.height/silkscreen.unit;
    return {int(std::floor((x+0.5f-rect.x)*width/rect.w)),int(std::floor((y+0.5f-rect.y)*height/rect.h))};
}
#endif
