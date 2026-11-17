@ Zodiac native (TNA) code resource header and entry for the TwGfx test program.
@ The ROM's TwLoadModule calls offset 0 twice: first with r0 = 0 and r1 = the TwGlue table, then with
@ r0 = emulStateP, r1 = the same table and r2 = Call68KFunc. r9 belongs to the OS and is never touched.
	.text
	.arm
	.global _start
_start:
	b	entry
	.word	0xe1a00000, 0xe1a00000, 0xe1a00000, 0xe1a00000, 0xe1a00000, 0xe1a00000
	.ascii	"ANWT"			@ +28, "TWNA" read as a word
	.word	1			@ +32 version
	.ascii	"rwdc"			@ +36
	.word	0, 0, 0, 0, 0, 0, 0, 0	@ +40..+71, CodeWarrior's section words, unused here

entry:
	stmfd	sp!, {r4-r8, sl, lr}
	cmp	r0, #0
	beq	done
	@ Second call: relocate once, then run the tests.
	adr	r4, _start
	ldr	r3, relocated
	cmp	r3, #0
	bne	run
	ldr	r5, relocOffset
	ldr	r6, relocCount
	add	r5, r5, r4
1:	subs	r6, r6, #1
	blt	2f
	ldr	r7, [r5], #4
	ldr	r8, [r4, r7]
	add	r8, r8, r4
	str	r8, [r4, r7]
	b	1b
2:	mov	r3, #1
	str	r3, relocated
run:
	mov	r0, r1
	bl	twgfxTestMain
done:
	mov	r0, #0
	ldmfd	sp!, {r4-r8, sl, lr}
	bx	lr

@ Patched by build.py: the relocation table's offset from _start and its word count. Each entry is the
@ offset of a word that holds an address linked at 0.
relocOffset:	.word	0x4f4c4552
relocCount:	.word	0x544e4352
relocated:	.word	0

@ TwGfxOpen for unsigned code: the ROM's own prologue, then a jump past its signed-app check into the
@ 1.1 ROM at 200995f0 (aResult in r6, aInfo in r7). The ROM's epilogue returns to our caller.
@ Same as the homebrew games (Spout, Thruster) do.
	.global	twGfxOpenUnsigned
twGfxOpenUnsigned:
	stmfd	sp!, {r4-r11, lr}
	ldr	r9, [r9]
	ldr	r9, [r9]
	sub	sp, sp, #36
	mov	r6, r0
	mov	r7, r1
	ldr	pc, [pc, #-4]
	.word	0x200995f0
