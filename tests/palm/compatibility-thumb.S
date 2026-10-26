/* PceNativeCall enters ARM state. Switch explicitly to the Thumb C function. */
    .section .text.compatibilityEntry,"ax"
    .align 2
    .arm
    .global compatibilityEntry
compatibilityEntry:
    adr ip, 1f
    add ip, ip, #1
    bx ip
    .thumb
    .thumb_func
1:
    b compatibilityNative
/* GCC 2.95 Thumb indirect-call helper, normally supplied by libgcc. BL sets
 * the Thumb return bit; BX selects the callback pointer's execution state. */
    .global _call_via_r8
    .thumb_func
_call_via_r8:
    bx r8
