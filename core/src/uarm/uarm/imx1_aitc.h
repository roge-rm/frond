#ifndef FROND_IMX1_AITC_H
#define FROND_IMX1_AITC_H

#include <cstdint>

enum class Imx1AitcAccess { ok, unmapped, width, reservedBits, externalInput };
enum class Imx1AitcLine { clear, asserted, unknown };

struct Imx1AitcPending {
    uint64_t normal, fast, unknownNormal, unknownFast;
};

// MC9328MX1RM chapter 10. Inputs are asserted-high logical peripheral requests.
class Imx1Aitc {
   public:
    Imx1AitcAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    void setInputs(uint64_t asserted, uint64_t known) {
        inputs = asserted & known & ~unusedInputs;
        knownInputs = known | unusedInputs;
    }
    bool interruptInputsKnown() const {
        const uint64_t normal = unknown(false);
        return !(normal && (normal & normalEligible())) && !unknown(true);
    }
    // What interruptInputsKnown gives after setInputs with these known inputs, whatever is asserted.
    bool interruptInputsKnownWith(uint64_t known) const {
        const uint64_t unknownInputs = ~(known | unusedInputs) & ~force & enabled;
        const uint64_t normal = unknownInputs & ~type;
        return !(normal && (normal & normalEligible())) && !(unknownInputs & type);
    }
    Imx1AitcLine irq() const;
    Imx1AitcLine fiq() const;
    Imx1AitcPending pendingState() const {
        return {pending(false), pending(true), unknown(false), unknown(true)};
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(control).Do32(mask).Do64(enabled).Do64(type).Do64(force).Do64(inputs).Do64(knownInputs);
        a.DoBuffer32(priority, 8);
    }
    static const char* registerName(uint32_t address);

   private:
    uint32_t control = 0, mask = 31;
    uint64_t enabled = 0, type = 0, force = 0, inputs = 0;
    // Table 10-1 reserves sources 31 and 32 for software forcing.
    static constexpr uint64_t unusedInputs = (uint64_t(1) << 31) | (uint64_t(1) << 32);
    uint64_t knownInputs = unusedInputs;
    uint32_t priority[8]{};
    uint64_t normalEligible() const;
    uint64_t pending(bool fast) const;
    uint64_t unknown(bool fast) const;
    uint32_t vector(bool fast) const;
    unsigned level(unsigned source) const { return (priority[source / 8] >> ((source % 8) * 4)) & 15; }
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Aitc) == 80, "update state() and stateVersion");
#endif

#endif
