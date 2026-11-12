//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _KEYS_H_
#define _KEYS_H_

#include <cstdint>
#include <functional>

#include "pxa_GPIO.h"

struct Keypad;

enum KeyId {
    keyInvalid = 0,
    keyIdHard1 = 1,
    keyIdHard2 = 2,
    keyIdHard3 = 3,
    keyIdHard4 = 4,
    keyIdUp = 5,
    keyIdDown = 6,
    keyIdLeft = 7,
    keyIdRight = 8,
    keyIdSelect = 9,
    keyIdPower = 10,
    // CLIE PEG-TG50 keys: Home and Graffiti in the app row, the side Jog Dial and Back,
    // then its 42 keyboard keys in rows from the top left.
    keyIdHome = 11,
    keyIdGraffiti = 12,
    keyIdJogUp = 13,
    keyIdJogDown = 14,
    keyIdJogPress = 15,
    keyIdBack = 16,
    keyIdKeyboard = 17,
    // Zire 72 side voice button (KPC direct key 7), after the TG50's keyboard keys.
    keyIdVoice = keyIdKeyboard + 42,
    keyIdHold = keyIdVoice + 1,
    // The connector's HotSync line (E2 GPIO4, TX GPIO10), pulled low while pressed.
    keyIdHotSync = keyIdHold + 1,
    // Tapwave Zodiac Function key and shoulder triggers. Its Home key is keyIdHome and its
    // Action keys are keyIdHard1-4.
    keyIdFunction = keyIdHotSync + 1,
    keyIdTriggerLeft = keyIdFunction + 1,
    keyIdTriggerRight = keyIdTriggerLeft + 1,
    // Pushing the Zodiac's analogue stick in.
    keyIdStickPress = keyIdTriggerRight + 1,
    // The Zodiac's Bluetooth button.
    keyIdBluetooth = keyIdStickPress + 1,
    keyIdCount = keyIdBluetooth + 1
};

// TG50 keyboard key at drive line 2-7 and column 0-6 of the ROM's keyboard code table; column c
// reads sense bit (c == 0 ? 1 : c == 1 ? 0 : c). Column 0 holds the modifiers and Tab.
constexpr KeyId tg50Key(int drive, int column) { return KeyId(keyIdKeyboard + (drive - 2) * 7 + column); }

struct Keypad *keypadInit(struct PxaGpio *gpio, bool matrixHasPullUps);
bool keypadDefineRow(struct Keypad *kp, unsigned rowIdx, int8_t gpio);
bool keypadDefineCol(struct Keypad *kp, unsigned colIdx, int8_t gpio);
bool keypadAddGpioKey(struct Keypad *kp, enum KeyId key, int8_t gpioNum, bool activeHigh);
bool keypadAddMatrixKey(struct Keypad *kp, enum KeyId key, unsigned row, unsigned col);

void keypadKeyEvt(struct Keypad *kp, enum KeyId key, bool wentDown);

void keypadReset(struct Keypad *kp);
std::function<void()> keypadCaptureInput(struct Keypad *kp);

#endif
