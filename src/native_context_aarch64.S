/* AAPCS64 cooperative context switch. x0=save, x1=load. */
.text
.align 2
.global mkda_context_swap
.type mkda_context_swap, %function
mkda_context_swap:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x2, sp
    str x2, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mrs x2, fpcr
    mrs x3, fpsr
    stp x2, x3, [x0, #168]
    b .Lrestore
.size mkda_context_swap, .-mkda_context_swap

.global mkda_context_jump
.type mkda_context_jump, %function
mkda_context_jump:
    mov x1, x0
.Lrestore:
    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ldr x2, [x1, #96]
    mov sp, x2
    ldp d8, d9, [x1, #104]
    ldp d10, d11, [x1, #120]
    ldp d12, d13, [x1, #136]
    ldp d14, d15, [x1, #152]
    ldp x2, x3, [x1, #168]
    msr fpcr, x2
    msr fpsr, x3
    mov x0, #0
    ret
.size mkda_context_jump, .-mkda_context_jump

.global mkda_context_entry
.type mkda_context_entry, %function
mkda_context_entry:
    blr x19
    /* The task trampoline must complete through mkda_context_jump. */
    brk #0
.size mkda_context_entry, .-mkda_context_entry
.section .note.GNU-stack,"",%progbits
