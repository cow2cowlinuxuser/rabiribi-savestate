/* thiscall thunks. eax = slot, ecx = wrap on entry to log_and_forward.
 * The real method is thiscall too: ecx is this, and it pops its own stack
 * args, so the stack must be exactly as the game left it when we jump. */
.section .text
.globl log_and_forward
log_and_forward:
	pushl %edx
	pushl %eax
	pushl %ecx
	pushl %eax
	pushl %ecx
	call _log_slot
	addl $8, %esp
	popl %ecx
	popl %eax
	popl %edx
	movl 4(%ecx), %ecx
	movl (%ecx), %edx
	/* eax is still the slot. Leave the real method with eax = this and
	 * edx = vtable, which is what the game's call instruction leaves. */
	pushl (%edx, %eax, 4)
	movl %ecx, %eax
	ret

.altmacro
.macro thunk_body n
.globl thunk_\n
thunk_\n:
	movl $\n, %eax
	jmp log_and_forward
.endm

.set i, 0
.rept 128
	thunk_body %i
	.set i, i+1
.endr

.macro table_entry n
	.long thunk_\n
.endm

.section .rdata,"dr"
.globl _thunk_table
_thunk_table:
.set i, 0
.rept 128
	table_entry %i
	.set i, i+1
.endr
