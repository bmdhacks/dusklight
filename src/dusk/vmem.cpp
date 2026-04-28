#include "dusk/vmem.h"

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#include <atomic>
#include <mutex>
#include <cstdint>

namespace dusk {

size_t vmem_page_size() {
#if _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

void* vmem_reserve(size_t size) {
#if _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
#endif
}

bool vmem_commit(void* ptr, size_t size) {
#if _WIN32
    return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(ptr, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

void vmem_decommit(void* ptr, size_t size) {
#if _WIN32
    VirtualFree(ptr, size, MEM_DECOMMIT);
#else
    mprotect(ptr, size, PROT_NONE);
    madvise(ptr, size, MADV_DONTNEED);
#endif
}

void vmem_release(void* ptr, size_t size) {
#if _WIN32
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}


namespace {

static void*               s_arenaBase  = nullptr;
static size_t              s_arenaTotal = 0;
static std::atomic<size_t> s_arenaBump{0};

struct FreeSlot { void* ptr; size_t size; };
static FreeSlot            s_free[JKR_VMEM_MAX_FREE_SLOTS];
static size_t              s_freeCount = 0;
static std::mutex          s_freeMutex;

} // namespace

void vmem_arena_init() {
    s_arenaBase  = vmem_reserve(JKR_VMEM_ARENA_SIZE);
    s_arenaTotal = JKR_VMEM_ARENA_SIZE;
}

void* vmem_arena_alloc(size_t size) {
    const size_t pageSize = vmem_page_size();
    size = (size + pageSize - 1) & ~(pageSize - 1);

    {
        std::lock_guard<std::mutex> lock(s_freeMutex);
        for (size_t i = 0; i < s_freeCount; ++i) {
            if (s_free[i].size >= size) {
                void* ptr = s_free[i].ptr;
                s_free[i] = s_free[--s_freeCount];
                return ptr;
            }
        }
    }

    size_t offset = s_arenaBump.fetch_add(size);
    if (offset + size > s_arenaTotal) {
        s_arenaBump.fetch_sub(size);
        return nullptr;
    }
    return static_cast<uint8_t*>(s_arenaBase) + offset;
}

void vmem_arena_free(void* ptr, size_t size) {
    if (!ptr) {
        return;
    }
    const size_t pageSize = vmem_page_size();
    size = (size + pageSize - 1) & ~(pageSize - 1);

    vmem_decommit(ptr, size);

    std::lock_guard<std::mutex> lock(s_freeMutex);
    if (s_freeCount < JKR_VMEM_MAX_FREE_SLOTS) {
        s_free[s_freeCount++] = {ptr, size};
    }
}

} // namespace dusk
