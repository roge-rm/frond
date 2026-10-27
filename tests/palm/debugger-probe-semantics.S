    .text
    .even
    .globl debuggerSemantics
    .globl debuggerException
/* a0 points at descriptor. Only caller-saved registers are used. Offsets are
 * fixed for the host acceptance oracle. No writable executable bytes. */
debuggerSemantics:
    move.l 4(%sp),%a0
    addq.l #1,36(%a0)
    move.l #0x01020304,44(%a0)
    moveq #7,%d0
    moveq #9,%d1
    add.l %d1,%d0
    move.l %d0,40(%a0)
    bra.s 1f
    moveq #-1,%d0
1:  addq.l #1,%d0
    move.l %d0,40(%a0)
    move.l %a0,-(%sp)
    move.l #3,-(%sp)
    pea 44(%a0)
    pea 45(%a0)
    trap #15
    .word 0xa026 /* MemMove(p+1,p,3): 01010203; replay => 01010102 */
    lea 12(%sp),%sp
    move.l (%sp)+,%a0
    rts

debuggerException:
    divu.w #0,%d0
    rts
