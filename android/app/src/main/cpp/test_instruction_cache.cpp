// Builds tests/instruction-cache.cpp into the app. The macro turns its main into a void
// function frond_instruction_cache, since main may end without a return and others may not.
#undef NDEBUG
#define main frond_unused_instruction_cache(); void frond_instruction_cache
#include "instruction-cache.cpp"
