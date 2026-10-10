/*
HOST_GL_TIMING.S

The shared half of the OpenGL call timer (host_gl_timing.c): each wrapped
function's stub loads its slot into x16 and branches here. The driver's
function runs with the caller's registers and stack untouched (x9 to x17
are free between calls), and the counter's ticks around it and the call go
into the slot: { function, calls, ticks, longest, longest's caller }.

GL runs on one thread, so the return address and start time wait in one
global save area rather than on the stack, which must stay as the caller
left it for arguments passed there.
*/

	.text
	.globl	host_gl_timing_common
	.type	host_gl_timing_common, %function
	.p2align 4
host_gl_timing_common:
	adrp	x15, host_gl_timing_save
	add	x15, x15, :lo12:host_gl_timing_save
	str	x30, [x15, #0]
	str	x16, [x15, #8]
	isb
	mrs	x17, cntvct_el0
	str	x17, [x15, #16]
	ldr	x17, [x16, #0]
	blr	x17
	isb
	mrs	x9, cntvct_el0
	adrp	x15, host_gl_timing_save
	add	x15, x15, :lo12:host_gl_timing_save
	ldr	x10, [x15, #16]
	sub	x9, x9, x10
	ldr	x16, [x15, #8]
	ldr	x10, [x16, #16]
	add	x10, x10, x9
	str	x10, [x16, #16]
	ldr	x10, [x16, #8]
	add	x10, x10, #1
	str	x10, [x16, #8]
	ldr	x30, [x15, #0]
	ldr	x10, [x16, #24]
	cmp	x9, x10
	b.ls	1f
	str	x9, [x16, #24]
	str	x30, [x16, #32]
1:
	ret
	.size	host_gl_timing_common, .-host_gl_timing_common

	.bss
	.p2align 4
	.globl	host_gl_timing_save
host_gl_timing_save:
	.zero	32

	.section .note.GNU-stack,"",%progbits
