#pragma once
#include <cstddef>

// Lua allocator for the player thread. Small blocks come from per-size free
// lists without taking KOS's malloc lock; larger ones use malloc.
void* dc_lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize);
