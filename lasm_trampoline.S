    .text
    .globl	_Lua_trampoline_func_start

// The C function signature for the JITted trampoline code is:
// void trampoline_code(lua_State *L, void *original_lua_executor_func_ptr);
//
// When called from C (e.g., from lua_G_trampoline_executor):
//   trampoline_entry(L, lua_G_original_executor);
//
// Calling convention for common ABIs (System V AMD64, AAPCS64):
// - 1st argument (lua_State *L): rdi (x86-64), x0 (aarch64), a0 (riscv)
// - 2nd ci
// - 3nd argument (original_lua_executor_func_ptr): rsi (x86-64), x1 (aarch64), a1 (riscv)
//
// The trampoline then calls original_lua_executor_func_ptr(L).
// The lua_State *L (already in rdi/x0/a0) is correctly positioned as the first argument
// for the call to original_lua_executor_func_ptr.

_Lua_trampoline_func_start:
#ifdef __x86_64__
    // rdi holds lua_State *L
    // rsi holds original_lua_executor_func_ptr
    
    // Ensure 16-byte stack alignment required by System V AMD64 ABI
    // When this function is called, the stack should already be 16-byte aligned
    // plus 8 bytes for the return address pushed by call instruction.
    // So we need to subtract 8 bytes to maintain 16-byte alignment before calling.
    sub     $8, %rsp       // Align stack to 16 bytes
    call    *%rdx          // Call original_lua_executor_func_ptr(L)
    add     $8, %rsp       // Restore stack pointer
    ret
#elif defined(__aarch64__) // ARM64
    // x0 holds lua_State *L
    // x1 holds original_lua_executor_func_ptr
    
    // Use tail call optimization - just jump to the target function
    // This preserves the original return address in the link register
    br      x1             // Branch to address in x1 (tail call)
#elif defined(__riscv) && __riscv_xlen == 64 // RV64G
    // a0 holds lua_State *L
    // a1 holds original_lua_executor_func_ptr
    
    // This is a tail call, so we can just jump.
    // The 'jr' instruction is a pseudo-instruction equivalent to 'jalr zero, rs1, 0'.
    jr      a1             // Jump to address in a1. 'a0' is passed as the first arg.
#else
    // Unsupported architecture - generate a compile-time error
    .error "Unsupported architecture for trampoline function"
#endif

    .globl	_Lua_trampoline_func_end
_Lua_trampoline_func_end:
    // Ensure the section is executable and has proper attributes for JIT code.
    // .balign 16 // Optional: align end for safety, though size calculation should handle it.
    .section        .note.GNU-stack,"",@progbits 