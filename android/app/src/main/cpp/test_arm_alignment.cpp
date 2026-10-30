// Builds tests/arm-alignment.cpp into the app. The macro turns its main into a void
// function frond_arm_alignment, since main may end without a return and others may not.
#undef NDEBUG
#define main frond_unused_arm_alignment(); void frond_arm_alignment
#include "arm-alignment.cpp"
