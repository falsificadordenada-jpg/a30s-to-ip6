/*
 * A30s hello-framebuffer: arm64 Linux Image header so the Samsung bootloader
 * treats us like a kernel. Position independent (everything PC-relative).
 * Entered at EL1 (or EL2) with MMU/D-cache off, x0 = DTB pointer (assumed).
 * Entry/exception structure adapted from dipa-studio/ios-sweet (MIT).
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset (same as the stock A30s kernel) */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	/* A secondary CPU that PSCI sent to the image header arrives here a 2nd time: boot_flag is
	 * already 1 (set by the primary, MMU/D-cache off so it went straight to DRAM). */
	adr	x1, boot_flag
	ldr	x2, [x1]
	cbnz	x2, secondary_entry
	mov	x2, #1
	str	x2, [x1]
	dsb	sy
	mov	x19, x0			/* DTB pointer from bootloader */
	mrs	x1, CurrentEL
	lsr	x1, x1, #2
	cmp	x1, #2
	b.eq	1f
	/* EL1 */
	msr	spsel, #1
	adr	x2, _stack_top
	mov	sp, x2
	adr	x2, vectors
	msr	vbar_el1, x2
	isb
	b	2f
1:	/* EL2 */
	adr	x2, _stack_top
	mov	sp, x2
	adr	x2, vectors
	msr	vbar_el2, x2
	isb
2:	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
3:	cmp	x1, x2
	b.hs	4f
	str	xzr, [x1], #8
	b	3b
4:	mov	x0, x19
	bl	hello_main
5:	wfe
	b	5b

/*
 * Exception vectors: every slot stashes x29/x30, loads its slot number and
 * jumps to the common path, which saves x0-x28 and calls exc_handler.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler		/* returns only when it recovered from a probed access fault */
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret

/*
 * ---- Secondary CPUs (M2d) ----
 * Entered by PSCI CPU_ON with MMU off. Does NOT trust x0 (context_id): the slot is found PC-relative
 * (slots_tab + idx*4096, idx = aff1*4+aff0), so any entry variant works.
 * Slot (u64 index): 0 status(1=MMU on) 1 mpidr 2 ttbr0 3 tcr 4 mair 5 vbar 6 stage(A1/A2/A3)
 *   7 CurrentEL 8 x0-as-received 9 sctlr-as-received 10 midr 12 esr 13 elr 14 exc-slot+1 15 far
 * FB bars (no memory reads needed), 28x8 px block at x = 32*idx:
 *   y1470 magenta = low trampoline ran, y1486 green = secondary_entry ran,
 *   y1502 yellow = about to enable MMU, y1518 cyan = MMU on.
 */
/* x12 = FB + idx*128 (x20) + row y */
.macro barptr y
	movz	x13, #(((\y) * 2880) & 0xffff)
	movk	x13, #((((\y) * 2880) >> 16) & 0xffff), lsl #16
	add	x12, x20, x13
.endm
/* paint a 28x8 block of colour w14 at x12 (strict-align safe, clobbers x15-x17) */
.macro paint
	mov	x15, #8
9:	mov	x16, x12
	mov	x17, #28
10:	str	w14, [x16], #4
	subs	x17, x17, #1
	b.ne	10b
	add	x12, x12, #2880
	subs	x15, x15, #1
	b.ne	9b
.endm
/* x20 = FB + 128*idx, x10 = idx */
.macro cpuidx
	mrs	x9, mpidr_el1
	ubfx	x10, x9, #0, #8
	ubfx	x11, x9, #8, #8
	add	x10, x10, x11, lsl #2
	and	x10, x10, #15
	movz	x20, #0xEC00, lsl #16
	add	x20, x20, x10, lsl #7
.endm

	.text
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	mov	x21, x0
	cpuidx
	barptr	1486
	movz	w14, #0xFF00, lsl #16
	movk	w14, #0xFF00			/* green */
	paint
	adr	x19, slots_tab
	add	x19, x19, x10, lsl #12
	mrs	x9, CurrentEL
	str	x9, [x19, #56]
	mrs	x9, mpidr_el1
	str	x9, [x19, #8]
	str	x21, [x19, #64]
	mrs	x9, sctlr_el1
	str	x9, [x19, #72]
	mrs	x9, midr_el1
	str	x9, [x19, #80]
	mov	x9, #0xA1
	str	x9, [x19, #48]
	dsb	sy
	add	x1, x19, #4096
	mov	sp, x1
	ldr	x2, [x19, #40]
	msr	vbar_el1, x2
	ldr	x2, [x19, #32]
	msr	mair_el1, x2
	ldr	x2, [x19, #24]
	msr	tcr_el1, x2
	ldr	x2, [x19, #16]
	msr	ttbr0_el1, x2
	dsb	sy
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
	ic	iallu
	dsb	nsh
	isb
	mov	x9, #0xA2
	str	x9, [x19, #48]
	dsb	sy
	barptr	1502
	movz	w14, #0xFFFF, lsl #16
	movk	w14, #0xFF00			/* yellow */
	paint
	mrs	x2, sctlr_el1
	orr	x2, x2, #1
	orr	x2, x2, #4
	orr	x2, x2, #(1 << 12)
	msr	sctlr_el1, x2
	isb
	barptr	1518
	movz	w14, #0xFF00, lsl #16
	movk	w14, #0xFFFF			/* cyan */
	paint
	mov	x9, #0xA3
	str	x9, [x19, #48]
	mov	x2, #1
	str	x2, [x19, #0]
	dc	civac, x19
	add	x1, x19, #64
	dc	civac, x1
	dsb	sy
8:	wfe
	b	8b

/*
 * Position-independent trampoline, copied by the primary to a low address (0x80080000) to test
 * "entry address must be low/kernel-like". Paints magenta, then jumps to the absolute address
 * patched into tramp_target.
 */
	.balign	8
	.globl tramp_start
tramp_start:
	msr	daifset, #0xf
	cpuidx
	barptr	1470
	movz	w14, #0xFFFF, lsl #16
	movk	w14, #0x00FF			/* magenta */
	paint
	ldr	x1, tramp_target
	br	x1
	.balign	8
	.globl tramp_target
tramp_target:
	.quad	0
	.globl tramp_end
tramp_end:

/* Secondary exception vectors: record ESR/ELR/FAR in the slot (x19) and park; never touch the screen. */
.macro svent n
	.balign	0x80
	mrs	x9, esr_el1
	str	x9, [x19, #96]
	mrs	x9, elr_el1
	str	x9, [x19, #104]
	mrs	x9, far_el1
	str	x9, [x19, #120]
	mov	x9, #(\n + 1)
	str	x9, [x19, #112]
	add	x9, x19, #64
	dc	civac, x9
	dsb	sy
11:	wfe
	b	11b
.endm
	.balign	0x800
	.globl svectors
svectors:
	svent 0
	svent 1
	svent 2
	svent 3
	svent 4
	svent 5
	svent 6
	svent 7
	svent 8
	svent 9
	svent 10
	svent 11
	svent 12
	svent 13
	svent 14
	svent 15

	.data
	.balign	8
	.globl boot_flag
boot_flag:
	.quad	0
