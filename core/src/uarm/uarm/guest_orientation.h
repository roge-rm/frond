#ifndef FROND_GUEST_ORIENTATION_H
#define FROND_GUEST_ORIENTATION_H

#include <cstdint>

// Which orientation the OS has chosen, from the display's scan orientation (SysGetOrientation numbers,
// 0 while the display is off) and the count of the guest's startups. The DAL logo is scanned unturned,
// which reads as portrait, so after any startup nothing counts until the OS first goes landscape. Sleep
// and wake turn the display off but are no startup, so a portrait program stays portrait.
class GuestOrientationTracker {
  public:
    // A new machine state: waits for landscape, or for a loaded session takes it as past its boot.
    void Restart(uint32_t startups) { seen = startups; booting = true; }
    void Loaded(uint32_t startups) { seen = startups; booting = false; }
    unsigned Update(unsigned scan, uint32_t startups) {
        if (startups != seen) { seen = startups; booting = true; }
        if (booting && scan == 2) booting = false;
        return booting ? 0 : scan;
    }

  private:
    uint32_t seen{0};
    bool booting{true};
};

#endif
