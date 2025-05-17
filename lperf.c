#define LUA_CORE

#include "lperf.h"

#if defined(LUA_HAVE_PERF_TRAMPOLINE)

#include "lobject.h"
#include "lstate.h"
#include "lvm.h"
#include "lfunc.h"
#include "ldebug.h"
#include "lapi.h" // For lua_lock/unlock
#include "lauxlib.h" // For luaL_newlib
#include "lmem.h" // For luaM_realloc_ and luaM_malloc

#include <stdio.h>
#include <string.h>
#include <stdlib.h> // For malloc, free, getenv, snprintf
#include <time.h>   // For time_t in some systems if pid_t is complex
#include <ctype.h>  // For tolower
#include <string.h> // Already included, but for strcasecmp if available, or manual lowercasing

// POSIX.1 standard header for getpid() and pid_t
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || (defined(__hpux) || defined(_AIX) && !defined(_WIN32))
#include <unistd.h> // Should provide pid_t and getpid()
#include <sys/types.h>
#elif defined(_WIN32)
#include <process.h> // For _getpid on Windows
  // Define pid_t for Windows if not already defined (it usually isn't a standard type here)
  // MSVC often uses int for process IDs from _getpid
  typedef int pid_t;
#define getpid _getpid
#else
  // Fallback for other systems - you might need to define pid_t and getpid appropriately
  // For example, if your system uses int for PIDs and has getpid():
  // typedef int pid_t;
  // extern int getpid(); // Or include the relevant header
  // As a last resort, a dummy implementation if PIDs are not critical for map name
  typedef int pid_t;
  static pid_t getpid_dummy(void) { return 0; } // Dummy getpid
#define getpid getpid_dummy
#warning "pid_t and getpid() are not fully defined for this platform in lperf.c. Using a dummy PID."
#endif


// --- Trampoline Definitions ---
// Adjust MAX_PERF_TRAMPOLINES and the definitions/array initialization below as needed.
#define MAX_PERF_TRAMPOLINES 32 // Example: Start with 32 trampolines

// Define a trampoline function. Its sole purpose is to be a unique address
// that calls the actual Lua VM execution function.
#define DEFINE_LUA_TRAMPOLINE(N) \
static void perf_trampoline_##N(lua_State *L, CallInfo *ci) { \
    /* This function's address is unique for perf. */ \
    /* It just calls the next step in Lua's VM execution for that CallInfo. */ \
    luaV_execute(L, ci); \
}

// Generate the trampoline functions
DEFINE_LUA_TRAMPOLINE(0)
DEFINE_LUA_TRAMPOLINE(1)
DEFINE_LUA_TRAMPOLINE(2)
DEFINE_LUA_TRAMPOLINE(3)
DEFINE_LUA_TRAMPOLINE(4)
DEFINE_LUA_TRAMPOLINE(5)
DEFINE_LUA_TRAMPOLINE(6)
DEFINE_LUA_TRAMPOLINE(7)
// Add more up to MAX_PERF_TRAMPOLINES-1
// For a large number, a script should generate these lines.
DEFINE_LUA_TRAMPOLINE(8) DEFINE_LUA_TRAMPOLINE(9) DEFINE_LUA_TRAMPOLINE(10) DEFINE_LUA_TRAMPOLINE(11)
DEFINE_LUA_TRAMPOLINE(12) DEFINE_LUA_TRAMPOLINE(13) DEFINE_LUA_TRAMPOLINE(14) DEFINE_LUA_TRAMPOLINE(15)
DEFINE_LUA_TRAMPOLINE(16) DEFINE_LUA_TRAMPOLINE(17) DEFINE_LUA_TRAMPOLINE(18) DEFINE_LUA_TRAMPOLINE(19)
DEFINE_LUA_TRAMPOLINE(20) DEFINE_LUA_TRAMPOLINE(21) DEFINE_LUA_TRAMPOLINE(22) DEFINE_LUA_TRAMPOLINE(23)
DEFINE_LUA_TRAMPOLINE(24) DEFINE_LUA_TRAMPOLINE(25) DEFINE_LUA_TRAMPOLINE(26) DEFINE_LUA_TRAMPOLINE(27)
DEFINE_LUA_TRAMPOLINE(28) DEFINE_LUA_TRAMPOLINE(29) DEFINE_LUA_TRAMPOLINE(30) DEFINE_LUA_TRAMPOLINE(31)


// Array of trampoline function pointers
static perf_trampoline_func_ptr all_perf_trampolines[MAX_PERF_TRAMPOLINES] = {
    perf_trampoline_0, perf_trampoline_1, perf_trampoline_2, perf_trampoline_3,
    perf_trampoline_4, perf_trampoline_5, perf_trampoline_6, perf_trampoline_7,
    perf_trampoline_8, perf_trampoline_9, perf_trampoline_10, perf_trampoline_11,
    perf_trampoline_12, perf_trampoline_13, perf_trampoline_14, perf_trampoline_15,
    perf_trampoline_16, perf_trampoline_17, perf_trampoline_18, perf_trampoline_19,
    perf_trampoline_20, perf_trampoline_21, perf_trampoline_22, perf_trampoline_23,
    perf_trampoline_24, perf_trampoline_25, perf_trampoline_26, perf_trampoline_27,
    perf_trampoline_28, perf_trampoline_29, perf_trampoline_30, perf_trampoline_31
    // Ensure this matches the DEFINE_LUA_TRAMPOLINE calls
};

static int next_perf_trampoline_idx = 0;

// Estimated size for a trampoline function's code.
// This is a rough estimate for perf.map.
#define PERF_TRAMPOLINE_CODE_SIZE 32 // Small, arbitrary size for the map file


static char* generate_perf_name(lua_State *L, const Proto *p) {
    lua_Debug ar;
    char buffer[512];
    char short_source_buf[256] = "unknown_source";
    const char *name = "?";
    int linedefined = 0;

    // To use lua_getinfo, we need the function on the stack.
    // We have the Proto *p. We can create a temporary LClosure on the stack.
    // This is a bit involved if we want to avoid heap allocation for the LClosure itself.
    // Alternative: try to use fields from Proto directly if sufficient.

    // Simpler approach: Directly use Proto fields, similar to original attempt,
    // but acknowledge limitations for C functions or complex scenarios where lua_getinfo shines.

    if (p->source) {
        const char *source_str = getstr(p->source);
        if (source_str) {
            const char *s = source_str;
            if (s[0] == '@') s++;
            else if (s[0] == '=') s++;

            const char *path_sep_fwd = strrchr(s, '/');
            const char *path_sep_bwd = strrchr(s, '\\');
            if (path_sep_fwd || path_sep_bwd) {
                s = (path_sep_fwd > path_sep_bwd ? path_sep_fwd : path_sep_bwd) + 1;
            }
            strncpy(short_source_buf, s, sizeof(short_source_buf) - 1);
            short_source_buf[sizeof(short_source_buf) - 1] = '\0';
        }
    }

    linedefined = p->linedefined;

    // For function name, luaG_getfunname was internal.
    // We can try to get some name info from Proto if available (e.g. for main chunk)
    // but it's not as reliable as lua_getinfo for all cases.
    // If p->pd_name is available and meaningful in your Lua variant for Protos, use it.
    // Otherwise, we stick to a placeholder or pointer for non-main chunks without debug info.

    // This part is tricky without luaG_getfunname or lua_getinfo on the Proto directly.
    // Let's try a simplified placeholder if no obvious name source from Proto.
    // Lua's 'luaG_addinfo' uses 'getfuncname' which checks p->name, then other sources.
    // 'p->name' itself is not part of the standard Proto in 5.4. We'll rely on line info.

    if (p->linedefined == 0) { // Often indicates main chunk or C func
         snprintf(buffer, sizeof(buffer), "lua:main_chunk_or_C@%s_P%p",
                 short_source_buf, (void*)p);
    } else {
        // No direct 'name' field in Proto. For named functions, debug info is separate.
        // We will use a placeholder for the name for now, relying on source and line.
        snprintf(buffer, sizeof(buffer), "lua:funcL%d@%s:%d",
                 p->linedefined, short_source_buf, p->linedefined);
    }

    char *result = (char *)luaM_realloc_(L, NULL, 0, strlen(buffer) + 1);
    if (result) strcpy(result, buffer);
    return result;
}

perf_trampoline_func_ptr get_or_create_trampoline_for_proto(lua_State *L, const Proto *p) {
    global_State *g = G(L);
    // No need to check g->perf_profiling_active here, assumed to be checked by caller or at entry

    // 1. Check if already registered
    for (int i = 0; i < g->perf_registry.count; i++) {
        if (g->perf_registry.entries[i].proto == p) {
            return g->perf_registry.entries[i].trampoline_func;
        }
    }

    // 2. If not, and trampolines are available, assign a new one
    if (next_perf_trampoline_idx >= MAX_PERF_TRAMPOLINES) {
        // Optional: log that we're out of trampolines
        // fprintf(stderr, "Lua perf: Out of trampolines!\n");
        return NULL;
    }

    perf_trampoline_func_ptr trampoline = all_perf_trampolines[next_perf_trampoline_idx];
    char *perf_name = generate_perf_name(L, p);
    if (!perf_name) return NULL; // Allocation failed

    // 3. Write to perf.map file
    if (g->perf_map_file) {
        // Note: Casting function pointer to void* for fprintf %p
        fprintf(g->perf_map_file, "%p %zu %s\n",
                (void *)trampoline, (size_t)PERF_TRAMPOLINE_CODE_SIZE, perf_name);
        fflush(g->perf_map_file); // Ensure it's written immediately
    }


    // 4. Add to registry
    if (g->perf_registry.count >= g->perf_registry.capacity) {
        int new_capacity = g->perf_registry.capacity == 0 ? 16 : g->perf_registry.capacity * 2;
        struct PerfProtoInfo *new_entries = (struct PerfProtoInfo *)luaM_realloc_(L,
            g->perf_registry.entries,
            g->perf_registry.capacity * sizeof(struct PerfProtoInfo),
            new_capacity * sizeof(struct PerfProtoInfo));

        if (!new_entries) {
            //fprintf(stderr, "Lua perf: Failed to realloc registry\n");
            luaM_free(L, perf_name);
            return NULL;
        }
        g->perf_registry.entries = new_entries;
        g->perf_registry.capacity = new_capacity;
    }

    g->perf_registry.entries[g->perf_registry.count].proto = p;
    g->perf_registry.entries[g->perf_registry.count].trampoline_func = trampoline;
    g->perf_registry.entries[g->perf_registry.count].perf_name_str = perf_name; // Ownership transferred
    g->perf_registry.count++;
    next_perf_trampoline_idx++;

    return trampoline;
}

static void free_perf_registry(lua_State *L) {
    global_State *g = G(L);
    if (g->perf_registry.entries) {
        for (int i = 0; i < g->perf_registry.count; i++) {
            if (g->perf_registry.entries[i].perf_name_str) {
                luaM_free(L, g->perf_registry.entries[i].perf_name_str);
            }
        }
        luaM_free(L, g->perf_registry.entries);
        g->perf_registry.entries = NULL;
    }
    g->perf_registry.count = 0;
    g->perf_registry.capacity = 0;
    next_perf_trampoline_idx = 0; // Reset for potential re-activation
}


void lua_perf_init_trampoline_pool(void) {
    // This function is mostly a placeholder if trampolines are statically defined.
    // Resetting the index might be useful if profiling is toggled.
    next_perf_trampoline_idx = 0;
}


LUA_API int lua_perf_setprofile(lua_State *L, int active) {
    global_State *g;
    lua_lock(L); // Ensure thread safety for global state modification
    g = G(L);

    if (active) {
        if (!g->perf_profiling_active) { // Initialize only if not already active
            lua_perf_init_trampoline_pool(); // Reset/prepare trampoline pool

            char map_filename[256]; // Increased buffer
            pid_t pid = getpid(); // Now pid_t should be defined
            // Using user's home or a configurable path might be better than /tmp/
            // For now, sticking to /tmp/ for simplicity as per common perf convention.
            #ifdef _WIN32
            // Windows doesn't have a standard /tmp equivalent easily accessible
            // for all users. Outputting to current directory for simplicity.
            // A more robust solution would use GetTempPath.
            snprintf(map_filename, sizeof(map_filename), "perf-%d.map", (int)pid);
            #else
            snprintf(map_filename, sizeof(map_filename), "/tmp/perf-%d.map", (int)pid);
            #endif

            free_perf_registry(L); // Clear any previous state

            g->perf_map_file = fopen(map_filename, "w");
            if (!g->perf_map_file) {
                // Optional: fprintf(stderr, "Lua perf: Failed to open %s\n", map_filename);
                lua_unlock(L);
                return 0; // Failure
            }
            g->perf_profiling_active = 1;
            // Optional: fprintf(stderr, "Lua perf: Profiling enabled, map file: %s\n", map_filename);
        }
    } else {
        if (g->perf_profiling_active) {
            g->perf_profiling_active = 0;
            if (g->perf_map_file) {
                fclose(g->perf_map_file);
                g->perf_map_file = NULL;
            }
            free_perf_registry(L); // Clean up
            // Optional: fprintf(stderr, "Lua perf: Profiling disabled.\n");
        }
    }
    lua_unlock(L);
    return 1; // Success
}

// Lua-callable functions for the 'perf' module
static int luaB_perf_enable(lua_State *L) {
    lua_perf_setprofile(L, 1);
    lua_pushboolean(L, G(L)->perf_profiling_active);
    return 1;
}

static int luaB_perf_disable(lua_State *L) {
    lua_perf_setprofile(L, 0);
    lua_pushboolean(L, G(L)->perf_profiling_active); // Push current state (should be 0)
    return 1;
}

static const luaL_Reg perf_funcs[] = {
    {"enable", luaB_perf_enable},
    {"disable", luaB_perf_disable},
    {NULL, NULL}
};

LUALIB_API int luaopen_perf (lua_State *L) {
  luaL_newlib(L, perf_funcs);

  // Initialize global state parts related to perf
  global_State *g = G(L);
  g->perf_profiling_active = 0;
  g->perf_map_file = NULL;
  g->perf_registry.entries = NULL;
  g->perf_registry.count = 0;
  g->perf_registry.capacity = 0;

  // Check for environment variable to enable profiling by default
  const char *env_val = getenv("LUA_PERF_PROFILE");
  if (env_val) {
    // Simple case-insensitive check for common true values
    char lower_env_val[16]; // Buffer for lowercase version
    int i = 0;
    for (i = 0; env_val[i] && i < sizeof(lower_env_val) - 1; i++) {
        lower_env_val[i] = tolower((unsigned char)env_val[i]);
    }
    lower_env_val[i] = '\0';

    if (strcmp(lower_env_val, "1") == 0 ||
        strcmp(lower_env_val, "true") == 0 ||
        strcmp(lower_env_val, "yes") == 0) {
      // Call lua_perf_setprofile to enable it. This will open the map file etc.
      // lua_perf_setprofile handles locking.
      lua_perf_setprofile(L, 1); 
    }
  }
  return 1;
}

#endif // LUA_HAVE_PERF_TRAMPOLINE