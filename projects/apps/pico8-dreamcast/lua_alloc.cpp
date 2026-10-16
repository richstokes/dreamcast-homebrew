// Lua allocates and frees constantly: strings, tables, closures and their
// growth. Every newlib malloc/free/realloc takes a KOS mutex, and KOS's
// mutex_unlock always searches the wait queues, so the lock alone cost about
// 7% of a busy cartridge's time on hardware. Only the player thread uses the
// Lua state, so small blocks can come from lock-free free lists instead.
//
// Lua passes the current size of every block it frees or resizes, so blocks
// carry no header: the size picks the free list. Memory in the free lists is
// kept for reuse rather than returned to malloc; the state lives as long as
// the player.
#include "lua_alloc.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {
constexpr size_t granule = 8;                  // Lua's maximum alignment
constexpr size_t small_limit = 256;            // larger blocks use malloc
constexpr size_t class_count = small_limit / granule;
constexpr size_t chunk_size = 16 * 1024;

struct FreeBlock { FreeBlock* next; };
FreeBlock* free_lists[class_count];
uint8_t* chunk_next = nullptr;
uint8_t* chunk_end = nullptr;

inline size_t size_class(size_t size) { return (size - 1) / granule; }

void* small_alloc(size_t size) {
    size_t index = size_class(size);
    if (FreeBlock* block = free_lists[index]) {
        free_lists[index] = block->next;
        return block;
    }
    size_t bytes = (index + 1) * granule;
    if (size_t(chunk_end - chunk_next) < bytes) {
        // The rest of the old chunk is too small for this class; give it to
        // the free list that fits it, so nothing is lost.
        size_t left = chunk_end - chunk_next;
        if (left >= granule) {
            auto* rest = reinterpret_cast<FreeBlock*>(chunk_next);
            size_t rest_index = left / granule - 1;
            rest->next = free_lists[rest_index];
            free_lists[rest_index] = rest;
        }
        auto* chunk = static_cast<uint8_t*>(malloc(chunk_size));
        if (!chunk) return nullptr;
        chunk_next = chunk;
        chunk_end = chunk + chunk_size;
    }
    void* block = chunk_next;
    chunk_next += bytes;
    return block;
}

inline void small_free(void* ptr, size_t size) {
    auto* block = static_cast<FreeBlock*>(ptr);
    size_t index = size_class(size);
    block->next = free_lists[index];
    free_lists[index] = block;
}
}

void* dc_lua_alloc(void*, void* ptr, size_t osize, size_t nsize) {
    // With no block, osize describes the kind of object, not a size.
    if (!ptr) osize = 0;
    bool old_small = ptr && osize <= small_limit;
    if (nsize == 0) {
        if (old_small) small_free(ptr, osize);
        else free(ptr);
        return nullptr;
    }
    if (nsize <= small_limit) {
        if (old_small && size_class(osize) == size_class(nsize)) return ptr;
        void* block = small_alloc(nsize);
        if (!block) return nullptr;     // Lua keeps the old block on failure
        if (ptr) {
            memcpy(block, ptr, osize < nsize ? osize : nsize);
            if (old_small) small_free(ptr, osize);
            else free(ptr);
        }
        return block;
    }
    if (!ptr || !old_small) return realloc(ptr, nsize);
    void* block = malloc(nsize);
    if (!block) return nullptr;
    memcpy(block, ptr, osize);
    small_free(ptr, osize);
    return block;
}
