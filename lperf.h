#ifndef lperf_h
#define lperf_h

#include "lua.h"

#if defined(LUA_HAVE_PERF_TRAMPOLINE)

// Forward declaration for the trampoline function pointer type
struct CallInfo;
struct Proto;
typedef void (*perf_trampoline_func_ptr)(lua_State *L, struct CallInfo *ci);

void lua_perf_init_trampoline_pool(void); // If explicit initialization is needed
perf_trampoline_func_ptr get_or_create_trampoline_for_proto(lua_State *L, const struct Proto *p);
LUA_API int lua_perf_setprofile(lua_State *L, int active);
LUALIB_API int luaopen_perf (lua_State *L);

#endif // LUA_HAVE_PERF_TRAMPOLINE

#endif