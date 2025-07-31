#ifndef luaperftrampoline_h
#define luaperftrampoline_h

#ifdef LUA_HAVE_PERF_TRAMPOLINE

#include "lua.h"
#include "lstate.h"  // For lua_State, Proto, CallInfo (Proto might be in lobject.h or lfunc.h depending on Lua version)
#include "lobject.h" // For Proto definition
#include <stddef.h>  // For size_t

// Forward declaration
typedef struct lua_code_arena_st lua_code_arena_t;

// Structure to manage memory arenas for trampolines
struct lua_code_arena_st {
    char *start_addr;    // Start of the memory arena
    char *current_addr;  // Address of the current trampoline within the arena
    size_t size;         // Size of the memory arena
    size_t size_left;    // Remaining size of the memory arena
    size_t trampoline_code_size; // Size of the assembly code for one trampoline
    lua_code_arena_t *prev; // Pointer to the previous arena
};

// Callback types for profiler integration (e.g., perf map output)
typedef void (*lua_perf_write_state_callback)(void *state, const void *code_addr,
                                             unsigned int code_size, Proto *p, lua_State *L);
typedef void* (*lua_perf_init_state_callback)(void);
typedef int (*lua_perf_free_state_callback)(void *state);

typedef struct lua_perf_callbacks_st {
    lua_perf_init_state_callback init_state;
    lua_perf_write_state_callback write_state;
    lua_perf_free_state_callback free_state;
    void *state; // Profiler-specific state (e.g., FILE* for perf map)
    size_t code_padding; // Extra padding after trampoline code, if any (usually 0)
} lua_perf_callbacks_t;

// Main API functions
// 'activate_immediately' if true, will try to hook into Lua's execution.
// Returns 0 on success, -1 on failure.
int lua_perf_trampoline_init(lua_State *L, int activate_immediately);
int lua_perf_trampoline_fini(lua_State *L); // Cleans up and unhooks
void lua_perf_trampoline_free_arenas(void); // Frees all mmaped arenas

// Function pointer type for the original Lua executor (e.g., a part of luaV_execute)
typedef void (*lua_original_executor_t)(lua_State *L, CallInfo *ci);

// This will be set by lua_perf_trampoline_init to point to the original Lua executor.
// The trampoline code will call this.
extern lua_original_executor_t lua_G_original_executor;

// The replacement function that Lua will call when trampolines are active.
// This function checks for/compiles a trampoline and then calls it.
void lua_G_trampoline_executor(lua_State *L, CallInfo *ci);

// Internal function to compile a trampoline for a Proto if it doesn't have one.
// Returns the executable trampoline address or NULL.
// The trampoline signature is: void (*trampoline_code)(lua_State *L, lua_original_executor_t original_func);
void *lua_perf_compile_trampoline_for_proto(lua_State *L, Proto *p);

// Default callbacks for perf map generation
extern lua_perf_callbacks_t lua_perf_default_map_callbacks;

// Symbols defined in lua_asm_trampoline.S, marking the start and end of the assembly template.
extern void _Lua_trampoline_func_start;
extern void _Lua_trampoline_func_end;

// Status of the trampoline system
#define LUA_PERF_STATUS_NO_INIT 0
#define LUA_PERF_STATUS_OK 1
#define LUA_PERF_STATUS_FAILED -1
#define LUA_PERF_STATUS_INACTIVE 2 // Initialized but not actively replacing executor

int lua_perf_get_status(void);
void lua_perf_activate(lua_State *L, int active); // Hook/unhook the executor

#endif // LUA_HAVE_PERF_TRAMPOLINE

#endif // luaperftrampoline_h 