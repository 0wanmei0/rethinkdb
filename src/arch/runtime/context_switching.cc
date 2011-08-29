#include "arch/runtime/context_switching.hpp"
#include "errors.hpp"
#include <sys/mman.h>
#ifdef VALGRIND
#include <valgrind/valgrind.h>
#endif

/* We have a custom implementation of `swapcontext()` that doesn't swap the
floating-point registers, the SSE registers, or the signal mask. This is for
performance reasons.

Our custom context-switching code is derived from GLibC, which is covered by the
LGPL. */

context_ref_t::context_ref_t() : pointer(NULL) { }

context_ref_t::~context_ref_t() {
    rassert(is_nil(), "You're leaking a context.");
}

bool context_ref_t::is_nil() {
    return pointer == NULL;
}

artificial_stack_t::artificial_stack_t(void (*initial_fun)(void), size_t _stack_size)
    : stack_size(_stack_size) {
    /* Allocate the stack */
    stack = malloc_aligned(stack_size, getpagesize());

    /* Protect the end of the stack so that we crash when we get a stack
    overflow instead of corrupting memory. */
    mprotect(stack, getpagesize(), PROT_NONE);

    /* Register our stack with Valgrind so that it understands what's going on
    and doesn't create spurious errors */
#ifdef VALGRIND
    valgrind_stack_id = VALGRIND_STACK_REGISTER(stack, (intptr_t)stack + stack_size);
#endif

    /* Set up the stack... */

    uint64_t *sp; /* A pointer into the stack. */

    /* Start at the beginning. */
    sp = reinterpret_cast<uint64_t *>(uintptr_t(stack) + stack_size);

    /* Align stack. The x86-64 ABI requires the stack pointer to always be
    16-byte-aligned at function calls. That is, "(%rsp - 8) is always a multiple
    of 16 when control is transferred to the function entry point". */
    sp = reinterpret_cast<uint64_t *>(uintptr_t(sp) & -16L);

    // Currently sp is 16-byte aligned.

    /* Set up the instruction pointer; this will be popped off the stack by ret
    in swapcontext once all the other registers have been "restored". */
    sp--;
    sp--;

    // Subtracted 2*sizeof(int64_t), so sp is still 16-byte aligned.

    *sp = reinterpret_cast<uint64_t>(initial_fun);

    /* These registers (r12, r13, r14, r15, rbx, rbp) are going to be popped off
    the stack by swapcontext; they're callee-saved, so whatever happens to be in
    them will be ignored. */
    sp -= 6;

    // Subtracted 6*sizeof(int64_t), so sp is still 16-byte aligned.

    /* Set up stack pointer. */
    context.pointer = sp;

    /* Our coroutines never return, so we don't put anything else on the stack.
    */
}

artificial_stack_t::~artificial_stack_t() {

    /* `context` must now point to what it was when we were created. If it
    doesn't, that means we're deleting the stack but the corresponding context
    is still "out there" somewhere. */
    rassert(!context.is_nil(), "we never got our context back");
    rassert(address_in_stack(context.pointer), "we got the wrong context back");

    /* Set `context.pointer` to nil to avoid tripping an assertion in its
    destructor. */
    context.pointer = NULL;

    /* Tell Valgrind the stack is no longer in use */
#ifdef VALGRIND
    VALGRIND_STACK_DEREGISTER(valgrind_stack_id);
#endif

    /* Undo protections changes */
    mprotect(stack, getpagesize(), PROT_READ|PROT_WRITE);

    /* Release the stack we allocated */
    free(stack);
}

bool artificial_stack_t::address_in_stack(void *addr) {
    return (uintptr_t)addr >= (uintptr_t)stack &&
        (uintptr_t)addr < (uintptr_t)stack + stack_size;
}

bool artificial_stack_t::address_is_stack_overflow(void *addr) {
    void *base = reinterpret_cast<void *>(floor_aligned(uintptr_t(addr), getpagesize()));
    return stack == base;
}

extern "C" {
/* `lightweight_swapcontext` is defined in the `asm` block further down */
extern void lightweight_swapcontext(void **current_pointer_out, void *dest_pointer);
}

void context_switch(context_ref_t *current_context_out, context_ref_t *dest_context_in) {

    rassert(current_context_out->is_nil(), "that variable already holds a context");
    rassert(!dest_context_in->is_nil(), "cannot switch to nil context");

    /* `lightweight_swapcontext()` won't set `dest_context_in->pointer` to NULL,
    so we have to do that ourselves. */
    void *dest_pointer = dest_context_in->pointer;
    dest_context_in->pointer = NULL;

    lightweight_swapcontext(&current_context_out->pointer, dest_pointer);
}

asm(
    /* `current_pointer_out` is in `%rdi`. `dest_pointer` is in `%rsi`. */

"lightweight_swapcontext:\n"

    /* Save preserved registers (the return address is already on the stack). */
    "pushq %r12\n"
    "pushq %r13\n"
    "pushq %r14\n"
    "pushq %r15\n"
    "pushq %rbx\n"
    "pushq %rbp\n"

    /* Save old stack pointer. */
    "movq %rsp, (%rdi)\n"

    /* Load the new stack pointer and the preserved registers. */
    "movq %rsi, %rsp\n"

    "popq %rbp\n"
    "popq %rbx\n"
    "popq %r15\n"
    "popq %r14\n"
    "popq %r13\n"
    "popq %r12\n"

    /* The following ret should return to the address set with
    `artificial_stack_t()` or with the previous `lightweight_swapcontext`. The
    instruction pointer is saved on the stack from the previous call (or
    initialized with `artificial_stack_t()`). */
    "ret\n"
);
