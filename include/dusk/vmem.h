#pragma once
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
namespace dusk {
#endif

// Reserve a contiguous virtual address range without committing physical pages
void* vmem_reserve(size_t size);

// Commit physical backing for pages in a previously reserved range, ptr and size should be page-aligned
bool vmem_commit(void* ptr, size_t size);

// Decommit physical pages in a reserved range, releasing RAM without releasing address space
void vmem_decommit(void* ptr, size_t size);

// Release an entire virtual reservation obtained from vmem_reserve
void vmem_release(void* ptr, size_t size);

// Returns the OS page size
size_t vmem_page_size();

// Shared vmem arena
// All JKR heap vmem reservations are sub-allocated from a single large reservation,
// keeping the total entry count at 1 regardless of how many heaps exist

// Must be called once before any JKR heap is created
void vmem_arena_init();

// Allocate a slot of size bytes (page-aligned) from the arena
void* vmem_arena_alloc(size_t size);

// Return a slot to the arena and decommit its physical pages
void vmem_arena_free(void* ptr, size_t size);

#ifdef __cplusplus
} // namespace dusk

// Total virtual address space reserved for the shared JKR heap arena
inline constexpr size_t JKR_VMEM_ARENA_SIZE = 128ULL * 1024 * 1024 * 1024; // 128 GB

// Virtual address space reserved per JKR heap (one slot in the shared arena)
inline constexpr size_t JKR_HEAP_VIRTUAL_RESERVE = 64ULL * 1024 * 1024; // 64 MB

// Minimum growth increment when a JKR heap expands into reserved but uncommitted pages
inline constexpr size_t JKR_HEAP_GROW_CHUNK = 16ULL * 1024 * 1024; // 16 MB

// Maximum number of free slots the arena can track (= total slots in the arena)
inline constexpr size_t JKR_VMEM_MAX_FREE_SLOTS = JKR_VMEM_ARENA_SIZE / JKR_HEAP_VIRTUAL_RESERVE;

#endif
