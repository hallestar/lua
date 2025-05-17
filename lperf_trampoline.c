#include "lperf_trampoline.h"

#ifdef LUA_HAVE_PERF_TRAMPOLINE

// Platform-specific includes must be within LUA_HAVE_PERF_TRAMPOLINE
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h> // For _getpid
#else // Not _WIN32 (assume POSIX-like)
#include <sys/mman.h>
#include <unistd.h>   // For sysconf, getpid
#include <sys/types.h> // For pid_t on some systems
#endif

#include "lauxlib.h" // For luaL_error
#include "lmem.h"    // For luaM_realloc, luaM_free
#include "lstate.h"  // Required for G(L) and global_State access
#include "lobject.h" // Required for Proto, Closure, ttisLclosure, s2v, clLvalue, getstr
#include "lvm.h"     // Potentially for CallInfo if not fully opaque via lstate.h
#include <stdio.h>    // For snprintf, fopen, fprintf, fclose, perror
#include <stdlib.h>   // For getenv, malloc, free
#include <string.h>   // For memcpy, strcmp, strerror
#include <assert.h>
#include <errno.h>    // For errno

// Global state for the trampoline system
static lua_code_arena_t *g_perf_code_arena = NULL;
static lua_perf_callbacks_t g_trampoline_api_callbacks;
static int g_perf_status = LUA_PERF_STATUS_NO_INIT;

lua_original_executor_t lua_G_original_executor = NULL;

static size_t round_up(size_t value, size_t multiple) {
    if (multiple == 0) return value;
    size_t remainder = value % multiple;
    if (remainder == 0) return value;
    return value + (multiple - remainder);
}

static int new_code_arena(lua_State *L) {
    size_t mem_size = 4096 * 16; // 64KB per arena
    size_t page_size_val = 4096;

#ifdef _WIN32
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    page_size_val = sysInfo.dwPageSize;
#else
    long ret_page_size = sysconf(_SC_PAGESIZE);
    if (ret_page_size > 0) page_size_val = (size_t)ret_page_size;
#endif

    if (mem_size % page_size_val != 0) {
        mem_size = round_up(mem_size, page_size_val);
    }

#ifdef _WIN32
    char *memory = (char*)VirtualAlloc(NULL, mem_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (memory == NULL) {
        fprintf(stderr, "Lua Perf Trampoline: VirtualAlloc failed. Error: %lu\n", GetLastError());
        g_perf_status = LUA_PERF_STATUS_FAILED;
        return -1;
    }
#else
    char *memory = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        perror("Lua Perf Trampoline: mmap failed");
        g_perf_status = LUA_PERF_STATUS_FAILED;
        return -1;
    }
#endif

    void *start_template = &_Lua_trampoline_func_start;
    void *end_template = &_Lua_trampoline_func_end;
    size_t code_size_one_trampoline = (char*)end_template - (char*)start_template;

    if (code_size_one_trampoline == 0 || code_size_one_trampoline > 1024 /*sanity check*/) {
        fprintf(stderr, "Lua Perf Trampoline: Invalid trampoline template size: %zu\n", code_size_one_trampoline);
#ifdef _WIN32
        VirtualFree(memory, 0, MEM_RELEASE);
#else
        munmap(memory, mem_size);
#endif
        g_perf_status = LUA_PERF_STATUS_FAILED;
        return -1;
    }

    size_t chunk_size = round_up(code_size_one_trampoline + g_trampoline_api_callbacks.code_padding, 16);
    size_t n_copies = mem_size / chunk_size;

    if (n_copies == 0) {
        fprintf(stderr, "Lua Perf Trampoline: Arena too small for even one trampoline (mem_size: %zu, chunk_size: %zu)\n", mem_size, chunk_size);
#ifdef _WIN32
        VirtualFree(memory, 0, MEM_RELEASE);
#else
        munmap(memory, mem_size);
#endif
        g_perf_status = LUA_PERF_STATUS_FAILED;
        return -1;
    }

    for (size_t i = 0; i < n_copies; i++) {
        memcpy(memory + i * chunk_size, start_template, code_size_one_trampoline);
    }

#ifdef _WIN32
    DWORD oldProtect;
    if (!VirtualProtect(memory, mem_size, PAGE_EXECUTE_READ, &oldProtect)) {
        fprintf(stderr, "Lua Perf Trampoline: VirtualProtect failed. Error: %lu\n", GetLastError());
        VirtualFree(memory, 0, MEM_RELEASE);
        g_perf_status = LUA_PERF_STATUS_FAILED;
        return -1;
    }
#else
    if (mprotect(memory, mem_size, PROT_READ | PROT_EXEC) == -1) {
        perror("Lua Perf Trampoline: mprotect failed");
        munmap(memory, mem_size);
        g_perf_status = LUA_PERF_STATUS_FAILED;
        return -1;
    }
#endif

#if (defined(__arm__) || defined(__aarch64__)) && (defined(__clang__) || defined(__GNUC__))
    // extern void __clear_cache(void* beg, void* end);
    // __clear_cache(memory, memory + mem_size);
#endif

    lua_code_arena_t *new_arena = (lua_code_arena_t*)malloc(sizeof(lua_code_arena_t));
    if (new_arena == NULL) {
        fprintf(stderr, "Lua Perf Trampoline: Failed to allocate arena metadata\n");
#ifdef _WIN32
        VirtualFree(memory, 0, MEM_RELEASE);
#else
        munmap(memory, mem_size);
#endif
        return -1;
    }

    new_arena->start_addr = memory;
    new_arena->current_addr = memory;
    new_arena->size = mem_size;
    new_arena->size_left = mem_size;
    new_arena->trampoline_code_size = code_size_one_trampoline;
    new_arena->prev = g_perf_code_arena;
    g_perf_code_arena = new_arena;
    return 0;
}

void lua_perf_trampoline_free_arenas(void) {
    lua_code_arena_t *cur = g_perf_code_arena;
    lua_code_arena_t *prev;
    while (cur) {
        prev = cur->prev;
#ifdef _WIN32
        if (cur->start_addr) VirtualFree(cur->start_addr, 0, MEM_RELEASE);
#else
        if (cur->start_addr) munmap(cur->start_addr, cur->size);
#endif
        free(cur);
        cur = prev;
    }
    g_perf_code_arena = NULL;
}

static void* code_arena_new_code(lua_code_arena_t *arena) {
    size_t total_code_size_with_padding = round_up(arena->trampoline_code_size + g_trampoline_api_callbacks.code_padding, 16);
    if (arena->size_left < total_code_size_with_padding) {
        return NULL;
    }
    void *trampoline = arena->current_addr;
    arena->current_addr += total_code_size_with_padding;
    arena->size_left -= total_code_size_with_padding;
    return trampoline;
}

void *lua_perf_compile_trampoline_for_proto(lua_State *L, Proto *p) {
    if (g_perf_status != LUA_PERF_STATUS_OK && g_perf_status != LUA_PERF_STATUS_INACTIVE) {
        return NULL;
    }
    if (p->perf_trampoline_entry != NULL) {
      return p->perf_trampoline_entry;
    }
    if (g_perf_code_arena == NULL || 
        g_perf_code_arena->size_left < round_up(g_perf_code_arena->trampoline_code_size + g_trampoline_api_callbacks.code_padding, 16)) {
        if (new_code_arena(L) != 0) {
            return NULL;
        }
    }
    void *trampoline = code_arena_new_code(g_perf_code_arena);
    if (trampoline == NULL) {
        fprintf(stderr, "Lua Perf Trampoline: Failed to get new code from arena despite available space check.\n");
        g_perf_status = LUA_PERF_STATUS_FAILED;
        return NULL;
    }
    if (g_trampoline_api_callbacks.write_state) {
        g_trampoline_api_callbacks.write_state(g_trampoline_api_callbacks.state, trampoline,
                                 g_perf_code_arena->trampoline_code_size, p, L);
    }
    p->perf_trampoline_entry = trampoline;
    return trampoline;
}

void lua_G_trampoline_executor(lua_State *L) {
    // If the original executor is not set, we are in a bad state.
    // This might happen if init wasn't called or failed very early.
    // Fallback to direct execution via G(L)->current_executor_func_ptr_for_perf
    // which should be the default luaV_execute_for_perf_trampoline.
    // However, if lua_G_original_executor IS set, it means we intended to use it.
    if (lua_G_original_executor == NULL) {
        // This case should ideally not be hit if init was successful and activate was called.
        // If it is hit, it means something is wrong with the init/activation logic.
        // For safety, try to call what's in current_executor_func_ptr_for_perf,
        // hoping it's the default luaV_execute_for_perf_trampoline.
        if (G(L)->current_executor_func_ptr_for_perf) {
             G(L)->current_executor_func_ptr_for_perf(L);
        }
        // If current_executor_func_ptr_for_perf is also NULL, Lua will likely crash,
        // but there's not much we can do here.
        return;
    }

    if (g_perf_status != LUA_PERF_STATUS_OK) { // Check if active and OK
        lua_G_original_executor(L); // Not active or error, call original
        return;
    }

    CallInfo *ci = L->ci;
    if (ci != NULL && ttisLclosure(s2v(ci->func.p))) {
        Closure *cl = clLvalue(s2v(ci->func.p));
        Proto *p = cl->l.p;
        if (p) {
            void *trampoline = p->perf_trampoline_entry;
            if (trampoline == NULL) {
                trampoline = lua_perf_compile_trampoline_for_proto(L, p);
            }
            if (trampoline) {
                ((void (*)(lua_State*, lua_original_executor_t))trampoline)(L, lua_G_original_executor);
                return;
            }
        }
    }
    lua_G_original_executor(L);
}

static FILE* g_perf_map_file = NULL;

static void default_perf_map_write_entry(void *state, const void *code_addr, 
                                         unsigned int code_size, Proto *p, lua_State *L) {
    FILE *f = (FILE*)state;
    if (!f || !p) return;
    const char *name = p->source ? getstr(p->source) : "unknown_source";
    if (p->linedefined > 0) {
        fprintf(f, "%lx %x lua::%s:%d\n", 
                (unsigned long)code_addr, 
                code_size, 
                name, 
                p->linedefined);
    } else {
        fprintf(f, "%lx %x lua::%s\n", 
                (unsigned long)code_addr, 
                code_size, 
                name);
    }
    fflush(f);
}

static void* default_perf_map_init_state(void) {
    if (g_perf_map_file != NULL) {
        fclose(g_perf_map_file);
        g_perf_map_file = NULL;
    }
    char filename[256];
#ifdef _WIN32
    int current_pid = _getpid();
#else
    pid_t current_pid = getpid();
#endif
    snprintf(filename, sizeof(filename), "/tmp/perf-%d.map", (int)current_pid);
    const char* map_env = getenv("LUA_PERF_MAP_FILE");
    if (map_env) {
        strncpy(filename, map_env, sizeof(filename) - 1);
        filename[sizeof(filename) - 1] = '\0';
    }
    g_perf_map_file = fopen(filename, "w");
    if (!g_perf_map_file) {
        perror("Lua Perf Trampoline: Could not open perf map file");
        return NULL;
    }
    return g_perf_map_file;
}

static int default_perf_map_free_state(void *state) {
    if (state) {
        fclose((FILE*)state);
        g_perf_map_file = NULL; // Clear global pointer
        return 0;
    }
    return -1;
}

lua_perf_callbacks_t lua_perf_default_map_callbacks = {
    default_perf_map_init_state,
    default_perf_map_write_entry,
    default_perf_map_free_state,
    NULL,
    0
};

int lua_perf_trampoline_init(lua_State *L, int activate_immediately) {
    if (g_perf_status != LUA_PERF_STATUS_NO_INIT && g_perf_status != LUA_PERF_STATUS_FAILED) {
        // Already initialized or in a failed state that wasn't reset
        return (g_perf_status == LUA_PERF_STATUS_FAILED) ? -1 : 0;
    }

    g_perf_status = LUA_PERF_STATUS_INACTIVE; // Initial status before activation

    // Store the original executor currently in global_State
    // This should be luaV_execute_for_perf_trampoline if lstate.c was modified correctly
    if (G(L)->current_executor_func_ptr_for_perf) {
        lua_G_original_executor = G(L)->current_executor_func_ptr_for_perf;
    } else {
        // This is a critical error: current_executor_func_ptr_for_perf should have been set by lua_newstate.
        // If it's NULL, the trampoline mechanism cannot work correctly.
        fprintf(stderr, "Lua Perf Trampoline CRITICAL: current_executor_func_ptr_for_perf is NULL during init! Check lstate.c modifications.\\n");
        g_perf_status = LUA_PERF_STATUS_FAILED;
        return -1;
    }
    
    // Initialize callbacks (example for perf map)
    g_trampoline_api_callbacks.init_state = default_perf_map_init_state;
    g_trampoline_api_callbacks.write_state = default_perf_map_write_entry;
    g_trampoline_api_callbacks.free_state = default_perf_map_free_state;
    g_trampoline_api_callbacks.code_padding = 0; // No extra padding by default
    g_trampoline_api_callbacks.state = g_trampoline_api_callbacks.init_state();

    if (g_trampoline_api_callbacks.state == NULL) {
        fprintf(stderr, "Lua Perf Trampoline: Failed to initialize perf map state.\\n");
        // continue, map file is not critical for trampolines themselves
    }

    // Check for environment variable activation override if not activating immediately
    if (!activate_immediately) {
        const char* env_activate = getenv("LUA_PERF_TRAMPOLINE_ACTIVATE");
        if (env_activate != NULL && (strcmp(env_activate, "1") == 0 || strcmp(env_activate, "true") == 0)) {
            activate_immediately = 1;
        }
    }

    if (activate_immediately) {
        lua_perf_activate(L, 1); // This will set g_perf_status to OK or FAILED
    } else {
        g_perf_status = LUA_PERF_STATUS_INACTIVE;
    }
    
    // new_code_arena is called on demand by lua_perf_compile_trampoline_for_proto
    // No need to pre-allocate it here unless desired for some reason.

    return (g_perf_status == LUA_PERF_STATUS_FAILED) ? -1 : 0;
}

int lua_perf_trampoline_fini(lua_State *L) {
    if (g_perf_status == LUA_PERF_STATUS_NO_INIT) {
        return 0; // Nothing to do
    }

    // Deactivate first, this will restore the original executor if active
    lua_perf_activate(L, 0);

    if (g_trampoline_api_callbacks.free_state && g_trampoline_api_callbacks.state) {
        g_trampoline_api_callbacks.free_state(g_trampoline_api_callbacks.state);
        g_trampoline_api_callbacks.state = NULL;
    }

    lua_perf_trampoline_free_arenas();
    
    // Clear the stored original executor
    lua_G_original_executor = NULL; 

    g_perf_status = LUA_PERF_STATUS_NO_INIT; // Reset status
    return 0;
}

int lua_perf_get_status(void) {
    return g_perf_status;
}

void lua_perf_activate(lua_State *L, int active) {
    if (active) {
        if (g_perf_status == LUA_PERF_STATUS_OK) return; // Already active and OK

        // It must be at least initialized (lua_G_original_executor must be set)
        if (lua_G_original_executor == NULL) {
             fprintf(stderr, "Lua Perf Trampoline: Cannot activate, original executor not saved. Init failed or was not called.\\n");
             g_perf_status = LUA_PERF_STATUS_FAILED; // Mark as failed
             return;
        }

        // Pre-allocate one arena to see if mmap/VirtualAlloc works
        if (g_perf_code_arena == NULL) { // Only if no arenas exist yet
            if (new_code_arena(L) != 0) {
                fprintf(stderr, "Lua Perf Trampoline: Failed to create initial code arena during activation.\\n");
                g_perf_status = LUA_PERF_STATUS_FAILED; // new_code_arena sets this, but be explicit
                // Restore original executor before failing activation
                if (G(L) && lua_G_original_executor) {
                    G(L)->current_executor_func_ptr_for_perf = lua_G_original_executor;
                }
                return;
            }
        }
        
        G(L)->current_executor_func_ptr_for_perf = lua_G_trampoline_executor;
        g_perf_status = LUA_PERF_STATUS_OK;
        // Write a marker to the perf map if available
        if (g_trampoline_api_callbacks.state && g_trampoline_api_callbacks.write_state) {
             FILE *f = (FILE*)g_trampoline_api_callbacks.state;
             fprintf(f, "0 0 lua::trampoline_system_activated\n");
             fflush(f);
        }

    } else { // Deactivating
        if (g_perf_status == LUA_PERF_STATUS_INACTIVE || g_perf_status == LUA_PERF_STATUS_NO_INIT) {
            return; // Already inactive or not initialized
        }
        
        if (lua_G_original_executor) {
            G(L)->current_executor_func_ptr_for_perf = lua_G_original_executor;
        } else {
            // This is problematic, implies init didn't run or failed to set lua_G_original_executor
            // We don't know what to restore to.
            // The default in lstate.c should be luaV_execute_for_perf_trampoline.
            // We might need a way to get that default if lua_G_original_executor is NULL.
            // For now, if it's NULL, we assume it implies a failed state and do nothing to the pointer.
            fprintf(stderr, "Lua Perf Trampoline: Cannot deactivate, original executor not known.\\n");
        }
        g_perf_status = LUA_PERF_STATUS_INACTIVE;
        // Write a marker to the perf map if available
        if (g_trampoline_api_callbacks.state && g_trampoline_api_callbacks.write_state) {
             FILE *f = (FILE*)g_trampoline_api_callbacks.state;
             fprintf(f, "0 0 lua::trampoline_system_deactivated\n");
             fflush(f);
        }
    }
}

#else // LUA_HAVE_PERF_TRAMPOLINE not defined

// Provide dummy implementations for the API functions if the feature is compiled out.
// This ensures that if other code unconditionally calls these functions, it will link.

int lua_perf_trampoline_init(lua_State *L, int activate_immediately) {
    (void)L; (void)activate_immediately; // Unused
    return 0; // No-op, success
}

int lua_perf_trampoline_fini(lua_State *L) {
    (void)L; // Unused
    return 0; // No-op, success
}

void lua_perf_trampoline_free_arenas(void) {
    // No-op
}

int lua_perf_get_status(void) {
    return LUA_PERF_STATUS_NO_INIT; // Or a specific status indicating not compiled in
}

void lua_perf_activate(lua_State *L, int active) {
    (void)L; (void)active; // Unused
    // No-op
}

// lua_G_trampoline_executor and lua_perf_compile_trampoline_for_proto are not directly
// called from outside if the feature is off and core Lua is guarded, so dummy 
// implementations for them are less critical unless there's a specific linkage need.
// extern void _Lua_trampoline_func_start; // These would cause linker errors if not defined
// extern void _Lua_trampoline_func_end;   // when LUA_HAVE_PERF_TRAMPOLINE is off.
                                        // But they are guarded in the .h file anyway.

#endif // LUA_HAVE_PERF_TRAMPOLINE 