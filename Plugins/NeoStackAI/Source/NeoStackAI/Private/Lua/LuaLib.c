// Compiles the entire Lua 5.4.7 library as a single C translation unit.
// UBT will compile this .c file with the C compiler, giving us C linkage
// that sol2 expects (SOL_USING_CXX_LUA=0).

// Export Lua C API symbols so extension modules (NSAI_PoseSearch, etc.) can link them.
// On Windows: dllexport. On Mac/Linux: visibility("default") to override -fvisibility=hidden.
// LUA_CORE is defined by the Lua sources themselves; we only need LUA_BUILD_AS_DLL here.
#define LUA_BUILD_AS_DLL

// UE 5.8 promoted MSVC C4702 ("unreachable code") to an error for third-party
// translation units. Vanilla Lua 5.4.7 deliberately includes a few unreachable
// fallback paths (e.g. ltm.c:217, lapi.c:1249, loslib.c:289) so that the static
// analyzer / cl /WX build does not flag them. Suppress just that warning around
// the amalgamated include without touching upstream Lua sources.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4702) // unreachable code
#endif

#include "LuaAll.c"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
