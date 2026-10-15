// Lua public APIs use C linkage; compile implementation as C++ for exception-safe host callbacks.
#pragma once
#ifndef LUA_CORE
#define LUA_CORE
#endif
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
