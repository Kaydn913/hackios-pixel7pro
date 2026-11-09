/*
 * fbtest: the first thing to run on the bare Pixel 7 Pro (cheetah), from `fastboot boot`.
 *
 * The bootloader treats us as a Linux arm64 Image: it loads us anywhere 2 MB aligned and enters at
 * offset 0 with the MMU off and x0 = its device tree. We have no console, so everything we learn is
 * painted on the framebuffer the bootloader left on (see main.c).
 */
	.section .text.head, "ax"
	.globl _start
_start:
	b	entry				/* code0 */
	.long	0				/* code1 */
	.quad	0				/* text_offset */
	.quad	_image_size			/* image_size: what the bootloader must leave us */
	.quad	0xa				/* flags: little endian, 4K pages, anywhere in RAM */
	.quad	0, 0, 0
	.ascii	"ARM\x64"			/* magic */
	.long	0

	.text
entry:
	adr	x1, _start			/* linked at 0x80000000: anywhere else, absolute pointers are wrong */
	ldr	x2, =_start
2:	cmp	x1, x2
	b.ne	2b				/* (the watchdog resets the phone) */
	mov	x20, x0				/* the device tree */
	msr	spsel, #1			/* sp is SP_ELx, the one exceptions to this EL use */
	mrs	x19, CurrentEL
	lsr	x19, x19, #2
	adrp	x1, stack_top
	add	x1, x1, :lo12:stack_top
	mov	sp, x1

	mov	x0, x19
	mov	x1, x20
	bl	at_entry			/* band 1: which EL the bootloader gave us */

	cmp	x19, #2
	b.ne	at_el1

	/*
	 * EL2 -> EL1, as Linux's el2_setup does when it is not going to stay a hypervisor. XNU here
	 * is built for EL1. HCR_EL2 goes first: it clears E2H if the bootloader left VHE on, so the
	 * registers below have their non-VHE layout.
	 */
	mov	x1, #(1 << 31)			/* HCR_EL2.RW: EL1 is AArch64; nothing trapped */
	msr	hcr_el2, x1
	isb
	mrs	x1, cnthctl_el2
	orr	x1, x1, #3			/* EL1PCTEN | EL1PCEN: EL1 may use the physical timer */
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr		/* virtual count == physical count */
	mov	x1, #0x33ff			/* CPTR_EL2: RES1 bits, no FP/SIMD trap */
	msr	cptr_el2, x1
	msr	hstr_el2, xzr
	mrs	x1, midr_el1			/* EL1 reads MIDR/MPIDR through these while EL2 exists */
	msr	vpidr_el2, x1
	mrs	x1, mpidr_el1
	msr	vmpidr_el2, x1
	mrs	x1, S3_4_C12_C9_5		/* ICC_SRE_EL2 */
	orr	x1, x1, #1			/* SRE */
	orr	x1, x1, #8			/* Enable: EL1 may use the GICv3 system registers */
	msr	S3_4_C12_C9_5, x1
	isb
	ldr	x1, =0x30d00800			/* SCTLR_EL1: RES1 bits; MMU and caches off */
	msr	sctlr_el1, x1
	mov	x1, sp
	msr	sp_el1, x1
	/*
	 * EL2 stays reachable: its vectors (el2.c's monitor, started by an HVC from EL1) and a stack of
	 * its own, since EL1 keeps using the one above.
	 */
	adr	x1, el2_vectors
	msr	vbar_el2, x1
	adrp	x1, el2_stack_top
	add	x1, x1, :lo12:el2_stack_top
	mov	sp, x1
	mov	x1, #0x3c5			/* SPSR: D, A, I, F masked; EL1h */
	msr	spsr_el2, x1
	adr	x1, at_el1
	msr	elr_el2, x1
	eret

at_el1:
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	mov	x1, x19
	mov	x2, x20
	bl	at_el1_main
1:	wfe
	b	1b

/*
 * EL1 exception vectors. Every entry saves the registers and calls the program's
 * u64 exception(u64 kind, u64 esr, u64 elr, u64 far): kind is the entry's number (0-15; 4 is
 * a synchronous exception from EL1 on SP_EL1, e.g. a register read that aborts). It returns the
 * address to resume at -- elr + 4 skips the faulting instruction -- or does not return.
 */
	.macro	vector kind
	.balign	128
	sub	sp, sp, #(34 * 8)
	stp	x0, x1, [sp, #0]
	mov	x0, #\kind
	b	vector_common
	.endm

	.balign	2048
vectors:
	.irp	k, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	vector	\k
	.endr

vector_common:
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
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	mrs	x1, esr_el1
	mrs	x2, elr_el1
	mrs	x3, far_el1
	bl	exception
	msr	elr_el1, x0
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
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	ldp	x0, x1, [sp, #0]
	add	sp, sp, #(34 * 8)
	eret

	.globl	psci_call
psci_call:					/* x0 = function, x1..x3 = arguments */
	smc	#0
	ret

	.globl	hvc_call
hvc_call:					/* x0 = function, x1..x3 = arguments: el2.c's monitor */
	hvc	#0
	ret

/*
 * Where a core that the kernel started with PSCI CPU_ON comes in, instead of the kernel's entry
 * (el2.c swaps it): at EL2, MMU off, x0 = the core's number. It gets a stack of its own, and
 * el2_secondary_setup() makes its EL2 like the boot core's and returns the kernel's context,
 * with ELR_EL2 at the kernel's entry: the core goes on at EL1, as the kernel expects.
 */
	.globl	el2_secondary
el2_secondary:
	msr	spsel, #1
	adrp	x1, el2_secondary_stacks
	add	x1, x1, :lo12:el2_secondary_stacks
	add	x1, x1, x0, lsl #12
	add	sp, x1, #4096
	bl	el2_secondary_setup
	eret

/*
 * EL2 exception vectors: every entry saves x0-x30 and ELR_EL2 in a frame on the EL2 stack and
 * calls el2.c's void el2_exception(u64 kind, struct el2_frame *f), which may change the frame's
 * registers and return address (an emulated read, a skipped instruction).
 */
	.macro	el2_vector kind
	.balign	128
	sub	sp, sp, #256
	stp	x0, x1, [sp, #0]
	mov	x0, #\kind
	b	el2_common
	.endm

	.balign	2048
	.globl	el2_vectors
el2_vectors:
	.irp	k, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
	el2_vector \k
	.endr

el2_common:
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
	stp	x28, x29, [sp, #224]
	mrs	x1, elr_el2
	stp	x30, x1, [sp, #240]
	mov	x1, sp
	bl	el2_exception
	ldp	x30, x1, [sp, #240]
	msr	elr_el2, x1
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
	ldp	x28, x29, [sp, #224]
	ldp	x0, x1, [sp, #0]
	add	sp, sp, #256
	eret

	.section .bss, "aw", %nobits
	.balign	16
	.space	16384
stack_top:
	.balign	16
	.space	16384
el2_stack_top:
	.balign	4096
el2_secondary_stacks:				/* 4K of EL2 stack per core, by core number */
	.space	8 * 4096
