// Built-in heaptrack-compatible allocation tracer for dusklight. See include/dusk/heaptrack.hpp.
//
// Native-ELF port of machismo/src/heaptrack_trace.c (itself from hashlink/src/gc_trace.c). Emits
// the heaptrack v1 interpreted text format via a FRAME-POINTER stack walk (x29 chain), NOT
// libunwind — upstream heaptrack's libunwind unwinder SIGSEGVs on aurora's GL/driver stacks. The
// machismo deltas dropped here: no guest/Mach-O layer (dusklight is a native ELF, symbolicated
// from its own .symtab + dladdr), allocations come from the process-wide exe-level interposers at
// the bottom of this file (dusklight uses glibc malloc directly — no shim).
//
// Everything is compiled only in a profiling build (-DDUSK_HEAPTRACK_BUILD=ON, which also passes
// -fno-omit-frame-pointer). A normal build gets the two no-op stubs below and nothing else.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "dusk/heaptrack.hpp"

#if !defined(DUSK_HEAPTRACK_BUILD) || defined(_WIN32)

namespace dusk::heaptrack {
void init() {}
void shutdown() {}
}  // namespace dusk::heaptrack

#else

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <ctime>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/syscall.h>

// DWARF/CFI unwinder — walks THROUGH the frame-pointer-less libstdc++/libc/driver frames that the
// frame-pointer walk can only fabricate past. UNW_LOCAL_ONLY selects the header-inlined fast path.
#define UNW_LOCAL_ONLY
#include <libunwind.h>

// Itanium C++ ABI demangler (libstdc++).
extern "C" char* __cxa_demangle(const char* mangled, char* out, size_t* len, int* status);

namespace {

// ---- Trace file state ----
FILE* trace_file = nullptr;
struct timespec trace_start_time;
int trace_alloc_count = 0;
int g_active = 0;

// One lock around the intern tables + writer; a per-thread guard so a hook never traces
// allocations made inside the hook itself.
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
__thread int g_in_hook = 0;

// ---- Output buffer ----
constexpr int TRACE_BUF_SIZE = 1 << 16;
char* trace_buf = nullptr;
int trace_buf_pos = 0;

void trace_flush() {
    if (trace_buf_pos > 0 && trace_file) {
        fwrite(trace_buf, 1, trace_buf_pos, trace_file);
        trace_buf_pos = 0;
    }
}

void trace_write(const char* data, int len) {
    if (trace_buf_pos + len > TRACE_BUF_SIZE)
        trace_flush();
    if (len > TRACE_BUF_SIZE) {
        fwrite(data, 1, len, trace_file);
        return;
    }
    memcpy(trace_buf + trace_buf_pos, data, len);
    trace_buf_pos += len;
}

void trace_printf(const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        trace_write(tmp, n);
}

// ---- Hashes ----
unsigned int hash_str(const char* s) {
    unsigned int h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h;
}
unsigned int hash_ptr(void* p) {
    uintptr_t v = (uintptr_t)p;
    v ^= v >> 16;
    v *= 0x45d9f3b;
    v ^= v >> 16;
    return (unsigned int)v;
}
unsigned int hash_trace(uintptr_t ip_hex, int parent) {
    uintptr_t v = ip_hex ^ ((uintptr_t)parent * 2654435761u);
    v ^= v >> 16;
    v *= 0x45d9f3b;
    v ^= v >> 16;
    return (unsigned int)v;
}
unsigned int hash_alloc_info(uint64_t size, int trace_idx) {
    uintptr_t v = (uintptr_t)size ^ ((uintptr_t)trace_idx * 2654435761u);
    v ^= v >> 16;
    v *= 0x9e3779b9;
    v ^= v >> 16;
    return (unsigned int)v;
}

// ==== String intern table: char* content -> 1-based index ====
struct str_entry {
    char* key;
    int index;
};
str_entry* str_table = nullptr;
int str_table_cap = 0, str_table_count = 0, str_next_index = 1;

void str_table_grow() {
    int new_cap = str_table_cap ? str_table_cap * 2 : 1024;
    auto* nt = (str_entry*)calloc(new_cap, sizeof(str_entry));
    for (int i = 0; i < str_table_cap; i++) {
        if (str_table[i].key) {
            unsigned int h = hash_str(str_table[i].key) & (new_cap - 1);
            while (nt[h].key)
                h = (h + 1) & (new_cap - 1);
            nt[h] = str_table[i];
        }
    }
    free(str_table);
    str_table = nt;
    str_table_cap = new_cap;
}

int intern_string(const char* s) {
    if (!s || !*s)
        s = "??";
    if (str_table_count * 10 >= str_table_cap * 7)
        str_table_grow();
    unsigned int h = hash_str(s) & (str_table_cap - 1);
    while (str_table[h].key) {
        if (strcmp(str_table[h].key, s) == 0)
            return str_table[h].index;
        h = (h + 1) & (str_table_cap - 1);
    }
    int idx = str_next_index++;
    char* copy = strdup(s);
    str_table[h].key = copy;
    str_table[h].index = idx;
    str_table_count++;
    trace_printf("s %s\n", copy);
    return idx;
}

// ==== Reverse ELF .symtab index for the main executable ====
// dusklight's own functions (mostly static -> not in .dynsym) symbolicate from here; shared-library
// frames fall back to dladdr. Built once at init from /proc/self/exe; the mapping is kept alive for
// the run (name pointers reference its .strtab).
struct ht_sym {
    uint64_t addr;
    const char* name;  // into the mapped exe .strtab (stable for the run)
};
ht_sym* g_syms = nullptr;
size_t g_nsyms = 0, g_syms_cap = 0;
int g_syms_sorted = 0;
const char* g_exe_mod = "dusklight";

constexpr uint64_t HT_MAX_SYM_DISTANCE = 1u << 20;  // 1 MB — caps mis-attribution of non-exe IPs

int ht_sym_cmp(const void* a, const void* b) {
    uint64_t va = ((const ht_sym*)a)->addr, vb = ((const ht_sym*)b)->addr;
    return va < vb ? -1 : (va > vb ? 1 : 0);
}

void build_elf_index() {
    // Main-executable load bias (0 for non-PIE; the mapped base for PIE).
    uintptr_t bias = 0;
    dl_iterate_phdr(
        [](struct dl_phdr_info* info, size_t, void* p) -> int {
            if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') {
                *(uintptr_t*)p = info->dlpi_addr;
                return 1;  // stop: main program found
            }
            return 0;
        },
        &bias);

    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return;
    }
    void* base = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED)
        return;

    auto* eh = (Elf64_Ehdr*)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
        return;  // (leak the mapping; harmless, one-shot)
    auto* sh = (Elf64_Shdr*)((char*)base + eh->e_shoff);
    Elf64_Shdr* symtab = nullptr;
    Elf64_Shdr* strtab = nullptr;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            symtab = &sh[i];
            strtab = &sh[sh[i].sh_link];
            break;
        }
    }
    if (!symtab || !strtab)
        return;

    auto* syms = (Elf64_Sym*)((char*)base + symtab->sh_offset);
    const char* strs = (const char*)base + strtab->sh_offset;
    size_t n = symtab->sh_size / sizeof(Elf64_Sym);
    for (size_t i = 0; i < n; i++) {
        if (ELF64_ST_TYPE(syms[i].st_info) != STT_FUNC)
            continue;
        if (syms[i].st_value == 0 || syms[i].st_shndx == SHN_UNDEF)
            continue;
        if (syms[i].st_name == 0)
            continue;
        if (g_nsyms == g_syms_cap) {
            size_t nc = g_syms_cap ? g_syms_cap * 2 : 8192;
            g_syms = (ht_sym*)realloc(g_syms, nc * sizeof(*g_syms));
            g_syms_cap = nc;
        }
        g_syms[g_nsyms].addr = bias + syms[i].st_value;
        g_syms[g_nsyms].name = strs + syms[i].st_name;
        g_nsyms++;
    }
    qsort(g_syms, g_nsyms, sizeof(*g_syms), ht_sym_cmp);
    g_syms_sorted = 1;
}

// Nearest preceding exe symbol. Async-signal-safe pure reads over the frozen sorted array.
int ht_lookup_sym(uint64_t ip, const char** out_fn) {
    if (!g_nsyms || !g_syms_sorted)
        return 0;
    size_t lo = 0, hi = g_nsyms;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_syms[mid].addr <= ip)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return 0;
    const ht_sym* s = &g_syms[lo - 1];
    if (ip - s->addr > HT_MAX_SYM_DISTANCE)
        return 0;
    *out_fn = s->name;
    return 1;
}

// ==== IP intern table: void* -> 1-based index ====
struct ip_entry {
    void* key;
    int index;
};
ip_entry* ip_table = nullptr;
int ip_table_cap = 0, ip_table_count = 0, ip_next_index = 1;

void ip_table_grow() {
    int new_cap = ip_table_cap ? ip_table_cap * 2 : 4096;
    auto* nt = (ip_entry*)calloc(new_cap, sizeof(ip_entry));
    for (int i = 0; i < ip_table_cap; i++) {
        if (ip_table[i].key) {
            unsigned int h = hash_ptr(ip_table[i].key) & (new_cap - 1);
            while (nt[h].key)
                h = (h + 1) & (new_cap - 1);
            nt[h] = ip_table[i];
        }
    }
    free(ip_table);
    ip_table = nt;
    ip_table_cap = new_cap;
}

// ---- Unlocked symbolication ----
// LOCK ORDER: g_lock is a LEAF lock. dladdr takes glibc's loader lock and glibc's dl* entry points
// malloc while holding it, so symbolication (dladdr + __cxa_demangle) runs OUTSIDE g_lock; intern_ip
// only consumes the pre-resolved results.
struct ht_ipres {
    void* addr;
    const char* fn;
    const char* mod;
    char* dem;  // owned __cxa_demangle buffer; freed by the hook
};

int lookup_ip(void* addr) {  // caller holds g_lock
    if (!ip_table_cap)
        return 0;
    unsigned int h = hash_ptr(addr) & (ip_table_cap - 1);
    while (ip_table[h].key) {
        if (ip_table[h].key == addr)
            return ip_table[h].index;
        h = (h + 1) & (ip_table_cap - 1);
    }
    return 0;
}

void ht_resolve_unlocked(ht_ipres* r) {
    r->fn = "??";
    r->mod = "??";
    r->dem = nullptr;
    const char* fn = nullptr;
    if (ht_lookup_sym((uint64_t)(uintptr_t)r->addr, &fn)) {  // exe .symtab (static fns)
        r->fn = fn;
        r->mod = g_exe_mod;
    } else {
        Dl_info info;
        if (dladdr(r->addr, &info)) {  // shared libs
            if (info.dli_fname)
                r->mod = info.dli_fname;
            if (info.dli_sname)
                r->fn = info.dli_sname;
        }
    }
    if (r->fn[0] == '_' && r->fn[1] == 'Z') {
        int status = 0;
        char* dem = __cxa_demangle(r->fn, nullptr, nullptr, &status);
        if (dem) {
            r->dem = dem;
            r->fn = dem;
        }
    }
}

constexpr int HT_MAX_DEPTH = 64;

int ht_presolve(void** frames, int n, ht_ipres* res) {
    int need = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < n; i++) {
        if (lookup_ip(frames[i]))
            continue;
        int dup = 0;
        for (int j = 0; j < need; j++)
            if (res[j].addr == frames[i]) {
                dup = 1;
                break;
            }
        if (!dup)
            res[need++].addr = frames[i];
    }
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < need; i++)
        ht_resolve_unlocked(&res[i]);
    return need;
}

void ht_ipres_free(ht_ipres* res, int nres) {
    for (int i = 0; i < nres; i++)
        free(res[i].dem);
}

int intern_ip(void* addr, const ht_ipres* res, int nres) {  // caller holds g_lock
    if (ip_table_count * 10 >= ip_table_cap * 7)
        ip_table_grow();
    unsigned int h = hash_ptr(addr) & (ip_table_cap - 1);
    while (ip_table[h].key) {
        if (ip_table[h].key == addr)
            return ip_table[h].index;
        h = (h + 1) & (ip_table_cap - 1);
    }
    int idx = ip_next_index++;
    ip_table[h].key = addr;
    ip_table[h].index = idx;
    ip_table_count++;

    const ht_ipres* r = nullptr;
    for (int j = 0; j < nres; j++)
        if (res[j].addr == addr) {
            r = &res[j];
            break;
        }
    int mod_idx = intern_string(r ? r->mod : "??");
    int fn_idx = intern_string(r ? r->fn : "??");
    int file_idx = intern_string("??");
    trace_printf("i %lx %x %x %x %x\n", (unsigned long)(uintptr_t)addr, mod_idx, fn_idx, file_idx, 0);
    return idx;
}

// ==== Trace-tree intern: (ip_hex, parent) -> 1-based index ====
struct trace_entry {
    uintptr_t ip_hex;
    int parent;
    int index;
};
trace_entry* trace_table = nullptr;
int trace_table_cap = 0, trace_table_count = 0, trace_next_index = 1;

void trace_table_grow() {
    int new_cap = trace_table_cap ? trace_table_cap * 2 : 4096;
    auto* nt = (trace_entry*)calloc(new_cap, sizeof(trace_entry));
    for (int i = 0; i < trace_table_cap; i++) {
        if (trace_table[i].index) {
            unsigned int h = hash_trace(trace_table[i].ip_hex, trace_table[i].parent) & (new_cap - 1);
            while (nt[h].index)
                h = (h + 1) & (new_cap - 1);
            nt[h] = trace_table[i];
        }
    }
    free(trace_table);
    trace_table = nt;
    trace_table_cap = new_cap;
}

int intern_trace(int ip_idx, uintptr_t ip_hex, int parent_trace) {
    if (trace_table_count * 10 >= trace_table_cap * 7)
        trace_table_grow();
    unsigned int h = hash_trace(ip_hex, parent_trace) & (trace_table_cap - 1);
    while (trace_table[h].index) {
        if (trace_table[h].ip_hex == ip_hex && trace_table[h].parent == parent_trace)
            return trace_table[h].index;
        h = (h + 1) & (trace_table_cap - 1);
    }
    int idx = trace_next_index++;
    trace_table[h].ip_hex = ip_hex;
    trace_table[h].parent = parent_trace;
    trace_table[h].index = idx;
    trace_table_count++;
    trace_printf("t %x %x\n", ip_idx, parent_trace);
    return idx;
}

// ==== Alloc-info intern: (size, trace_idx) -> 0-based index ====
struct ainfo_entry {
    uint64_t size;
    int trace_idx;
    int index;  // -1 = empty
};
ainfo_entry* ainfo_table = nullptr;
int ainfo_table_cap = 0, ainfo_table_count = 0, ainfo_next_index = 0;

void ainfo_table_grow() {
    int new_cap = ainfo_table_cap ? ainfo_table_cap * 2 : 4096;
    auto* nt = (ainfo_entry*)malloc(new_cap * sizeof(ainfo_entry));
    for (int i = 0; i < new_cap; i++)
        nt[i].index = -1;
    for (int i = 0; i < ainfo_table_cap; i++) {
        if (ainfo_table[i].index >= 0) {
            unsigned int h = hash_alloc_info(ainfo_table[i].size, ainfo_table[i].trace_idx) & (new_cap - 1);
            while (nt[h].index >= 0)
                h = (h + 1) & (new_cap - 1);
            nt[h] = ainfo_table[i];
        }
    }
    free(ainfo_table);
    ainfo_table = nt;
    ainfo_table_cap = new_cap;
}

int intern_alloc_info(uint64_t size, int trace_idx) {
    if (ainfo_table_count * 10 >= ainfo_table_cap * 7)
        ainfo_table_grow();
    unsigned int h = hash_alloc_info(size, trace_idx) & (ainfo_table_cap - 1);
    while (ainfo_table[h].index >= 0) {
        if (ainfo_table[h].size == size && ainfo_table[h].trace_idx == trace_idx)
            return ainfo_table[h].index;
        h = (h + 1) & (ainfo_table_cap - 1);
    }
    int idx = ainfo_next_index++;
    ainfo_table[h].size = size;
    ainfo_table[h].trace_idx = trace_idx;
    ainfo_table[h].index = idx;
    ainfo_table_count++;
    trace_printf("a %lx %x\n", (unsigned long)size, trace_idx);
    return idx;
}

// ==== Pointer map: void* -> alloc_info_index (for frees) ====
struct ptr_entry {
    void* key;  // NULL = empty
    int alloc_info_idx;
};
ptr_entry* ptr_table = nullptr;
int ptr_table_cap = 0, ptr_table_count = 0;

void ptr_map_insert(void* ptr, int alloc_info_idx);

void ptr_table_grow() {
    int new_cap = ptr_table_cap ? ptr_table_cap * 2 : (1 << 16);
    auto* nt = (ptr_entry*)calloc(new_cap, sizeof(ptr_entry));
    for (int i = 0; i < ptr_table_cap; i++) {
        if (ptr_table[i].key) {
            unsigned int h = hash_ptr(ptr_table[i].key) & (new_cap - 1);
            while (nt[h].key)
                h = (h + 1) & (new_cap - 1);
            nt[h] = ptr_table[i];
        }
    }
    free(ptr_table);
    ptr_table = nt;
    ptr_table_cap = new_cap;
}

void ptr_map_insert(void* ptr, int alloc_info_idx) {
    if (ptr_table_count * 10 >= ptr_table_cap * 7)
        ptr_table_grow();
    unsigned int h = hash_ptr(ptr) & (ptr_table_cap - 1);
    while (ptr_table[h].key) {
        if (ptr_table[h].key == ptr) {
            ptr_table[h].alloc_info_idx = alloc_info_idx;
            return;
        }
        h = (h + 1) & (ptr_table_cap - 1);
    }
    ptr_table[h].key = ptr;
    ptr_table[h].alloc_info_idx = alloc_info_idx;
    ptr_table_count++;
}

int ptr_map_remove(void* ptr) {
    if (!ptr_table_cap)
        return -1;
    unsigned int h = hash_ptr(ptr) & (ptr_table_cap - 1);
    while (ptr_table[h].key) {
        if (ptr_table[h].key == ptr) {
            int idx = ptr_table[h].alloc_info_idx;
            ptr_table[h].key = nullptr;
            ptr_table_count--;
            unsigned int j = (h + 1) & (ptr_table_cap - 1);
            while (ptr_table[j].key) {
                void* k = ptr_table[j].key;
                int v = ptr_table[j].alloc_info_idx;
                ptr_table[j].key = nullptr;
                ptr_table_count--;
                ptr_map_insert(k, v);
                j = (j + 1) & (ptr_table_cap - 1);
            }
            return idx;
        }
        h = (h + 1) & (ptr_table_cap - 1);
    }
    return -1;
}

// ==== Synthetic tag IPs: category leaf frames for mmap events ====
constexpr int HT_TAG_MAX = 32;
struct {
    char* tag;
    int ip_idx;
    uintptr_t addr;
} tag_ips[HT_TAG_MAX];
int tag_ip_count = 0;

int intern_tag_ip(const char* tag, uintptr_t* out_addr) {
    for (int i = 0; i < tag_ip_count; i++) {
        if (strcmp(tag_ips[i].tag, tag) == 0) {
            *out_addr = tag_ips[i].addr;
            return tag_ips[i].ip_idx;
        }
    }
    int slot = tag_ip_count;
    if (slot == HT_TAG_MAX - 1)
        tag = "mmap(other)";
    uintptr_t addr = 0xfff0000000000000ull + (uintptr_t)slot + 1;
    int mod_idx = intern_string("<mmap>");
    int fn_idx = intern_string(tag);
    int file_idx = intern_string("??");
    int idx = ip_next_index++;
    trace_printf("i %lx %x %x %x %x\n", (unsigned long)addr, mod_idx, fn_idx, file_idx, 0);
    tag_ips[slot].tag = strdup(tag);
    tag_ips[slot].ip_idx = idx;
    tag_ips[slot].addr = addr;
    tag_ip_count++;
    *out_addr = addr;
    return idx;
}

// ==== Tracked mmap regions (for munmap, incl. partial unmaps) ====
struct ht_region {
    char* addr;
    size_t len;
    int ainfo_idx;
    int trace_idx;
};
ht_region* regions = nullptr;
int region_count = 0, region_cap = 0;

void region_add(char* addr, size_t len, int ainfo_idx, int trace_idx) {
    if (region_count == region_cap) {
        region_cap = region_cap ? region_cap * 2 : 256;
        regions = (ht_region*)realloc(regions, region_cap * sizeof(*regions));
    }
    regions[region_count] = {addr, len, ainfo_idx, trace_idx};
    region_count++;
}

// ---- RSS ground truth: periodic `R <resident pages>` (statm field 2) ----
void emit_rss_locked() {
    char buf[128];
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    unsigned long vsz = 0, rss = 0;
    if (sscanf(buf, "%lu %lu", &vsz, &rss) == 2 && rss)
        trace_printf("R %lx\n", rss);
}

// ---- Frame-pointer walk ----
// Drop 2 tracer frames (ht_alloc/ht_realloc/ht_mmap AND the malloc/calloc/... interposer) so the
// leaf is the real allocating CALL SITE, not the primitive. Critical for grouping: with libunwind
// every alloc's leaf is otherwise the interposer ("malloc"), collapsing millions of distinct call
// sites into one useless node. Every primitive path is exactly interposer -> ht_* -> ht_capture.
constexpr int HT_SKIP_FRAMES = 2;
__thread void* t_stk_lo;
__thread void* t_stk_hi;
__thread int t_stk_init;

// Strip ARM Pointer Authentication from a return address. On PAC cores (Asahi/M2 sign LR with
// -mbranch-protection=pac-ret) the saved LR carries a signature in bits 48-63; clear them to get
// the real 48-bit VA. No-op on non-PAC targets (Mali handhelds: addresses already < 2^48).
static inline void* ht_strip_pac(void* p) {
#if defined(__aarch64__)
    return (void*)((uintptr_t)p & 0x0000FFFFFFFFFFFFull);
#else
    return p;
#endif
}

// Frame-pointer fallback: used when libunwind faults (guard fires) or returns nothing. Only walks
// through code compiled with -fno-omit-frame-pointer (our game + aurora); fabricates past FP-less
// library frames, so it's the last resort, not the primary.
__attribute__((noinline)) int ht_capture_fp(void** buf, int max) {
    void** fp = (void**)__builtin_frame_address(0);
    if (!t_stk_init) {
        pthread_attr_t a;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            void* base;
            size_t sz;
            if (pthread_attr_getstack(&a, &base, &sz) == 0) {
                t_stk_lo = base;
                t_stk_hi = (char*)base + sz;
            }
            pthread_attr_destroy(&a);
        }
        t_stk_init = 1;
    }
    void* lo = nullptr;
    void* hi = nullptr;
    if (t_stk_hi && (void*)fp >= t_stk_lo && (void*)fp < t_stk_hi) {
        lo = t_stk_lo;
        hi = t_stk_hi;
    }
    int depth = 0;
    while (fp && depth < max) {
        void* lr = ht_strip_pac(fp[1]);  // saved LR (PAC-stripped; Asahi/M2 signs return addresses)
        if (!lr)
            break;
        buf[depth++] = lr;
        void** next = (void**)fp[0];  // saved x29
        if (next <= fp)
            break;
        if ((uintptr_t)next & 0xF)
            break;  // arm64 x29 is 16-byte aligned
        if (hi && (void*)next >= hi)
            break;
        if (lo && (void*)next < lo)
            break;
        fp = next;
    }
    return depth;
}

// ---- Guarded libunwind capture (primary) ----
// unw_step CAN SIGSEGV walking the tableless GL/driver stacks (the reason stock heaptrack crashed
// on this game). So the whole unwind runs under a per-thread SIGSEGV/SIGBUS guard that siglongjmps
// out and falls back to the frame-pointer walk. Scoped by t_unwinding: a fault OUTSIDE our unwind
// chains to whatever handler was installed before us, so real crashes are unaffected. Caching is
// disabled (UNW_CACHE_NONE) so a longjmp mid-step can't strand a libunwind cache lock.
__thread sigjmp_buf t_unwind_jmp;
__thread volatile sig_atomic_t t_unwinding;
struct sigaction g_old_segv, g_old_bus;
int g_unwind_guard_installed;
unsigned long g_unwind_faults;  // diagnostic: how often libunwind faulted -> FP fallback (racy count is fine)

void ht_unwind_signal_handler(int sig, siginfo_t* si, void* uctx) {
    if (t_unwinding) {
        t_unwinding = 0;
        siglongjmp(t_unwind_jmp, 1);
    }
    // Not from our unwind — defer to whoever held the handler before us.
    struct sigaction* old = (sig == SIGBUS) ? &g_old_bus : &g_old_segv;
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
        old->sa_sigaction(sig, si, uctx);
    } else if (!(old->sa_flags & SA_SIGINFO) && old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

void ht_install_unwind_guard() {
    if (g_unwind_guard_installed)
        return;
    // Per-thread caching: fast (uncached DWARF unwind per alloc is ~10x too slow to reach gameplay),
    // and no cross-thread cache lock that a mid-step longjmp could strand into a deadlock.
    unw_set_caching_policy(unw_local_addr_space, UNW_CACHE_PER_THREAD);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ht_unwind_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
    g_unwind_guard_installed = 1;
}

__attribute__((noinline)) int ht_capture(void** buf, int max) {
    int depth = 0;  // only read on the fault-free path (the else/longjmp path ignores it)
    if (g_unwind_guard_installed && sigsetjmp(t_unwind_jmp, 1) == 0) {
        t_unwinding = 1;
        unw_context_t uc;
        unw_cursor_t cur;
        if (unw_getcontext(&uc) == 0 && unw_init_local(&cur, &uc) == 0) {
            while (depth < max && unw_step(&cur) > 0) {
                unw_word_t ip = 0;
                unw_get_reg(&cur, UNW_REG_IP, &ip);
                if (!ip)
                    break;
                buf[depth++] = ht_strip_pac((void*)(uintptr_t)ip);  // idempotent if libunwind already stripped
            }
        }
        t_unwinding = 0;
        if (depth > 0)
            return depth;
    } else {
        t_unwinding = 0;  // faulted mid-unwind; fall through to the frame-pointer walk
        g_unwind_faults++;
    }
    return ht_capture_fp(buf, max);
}

long trace_elapsed_ms() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - trace_start_time.tv_sec) * 1000L + (now.tv_nsec - trace_start_time.tv_nsec) / 1000000L;
}

// ---- Core emit (caller holds g_lock + g_in_hook; frames captured/presolved before locking) ----
void do_alloc_locked(void* ptr, size_t size, void** frames, int nframes, const ht_ipres* res, int nres) {
    int skip = nframes > HT_SKIP_FRAMES ? HT_SKIP_FRAMES : 0;
    int parent_trace = 0;
    for (int i = nframes - 1; i >= skip; i--) {
        int ip_idx = intern_ip(frames[i], res, nres);
        parent_trace = intern_trace(ip_idx, (uintptr_t)frames[i], parent_trace);
    }
    int ainfo_idx = intern_alloc_info((uint64_t)size, parent_trace);
    ptr_map_insert(ptr, ainfo_idx);
    if ((++trace_alloc_count & 0x3FF) == 0) {
        trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
        emit_rss_locked();
        trace_flush();
        fflush(trace_file);  // survive a hard kill (the game's dev-quit never runs atexit)
    }
    trace_printf("+ %x\n", ainfo_idx);
}

void do_mmap_locked(void* ptr, size_t len, const char* tag, void** frames, int nframes, const ht_ipres* res, int nres) {
    int skip = nframes > HT_SKIP_FRAMES ? HT_SKIP_FRAMES : 0;
    int parent_trace = 0;
    for (int i = nframes - 1; i >= skip; i--) {
        int ip_idx = intern_ip(frames[i], res, nres);
        parent_trace = intern_trace(ip_idx, (uintptr_t)frames[i], parent_trace);
    }
    uintptr_t tag_addr;
    int tag_ip = intern_tag_ip(tag, &tag_addr);
    parent_trace = intern_trace(tag_ip, tag_addr, parent_trace);
    int ainfo_idx = intern_alloc_info((uint64_t)len, parent_trace);
    region_add((char*)ptr, len, ainfo_idx, parent_trace);
    trace_printf("+ %x\n", ainfo_idx);
    trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
    emit_rss_locked();
    trace_flush();
    fflush(trace_file);
}

void do_munmap_locked(void* ptr, size_t len) {
    char* us = (char*)ptr;
    char* ue = us + len;
    int hit = 0;
    for (int i = 0; i < region_count;) {
        ht_region r = regions[i];
        char* rs = r.addr;
        char* re = rs + r.len;
        if (re <= us || rs >= ue) {
            i++;
            continue;
        }
        hit = 1;
        trace_printf("- %x\n", r.ainfo_idx);
        regions[i] = regions[--region_count];
        if (rs < us) {
            int a = intern_alloc_info((uint64_t)(us - rs), r.trace_idx);
            region_add(rs, (size_t)(us - rs), a, r.trace_idx);
            trace_printf("+ %x\n", a);
        }
        if (re > ue) {
            int a = intern_alloc_info((uint64_t)(re - ue), r.trace_idx);
            region_add(ue, (size_t)(re - ue), a, r.trace_idx);
            trace_printf("+ %x\n", a);
        }
    }
    if (hit) {
        trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
        emit_rss_locked();
        trace_flush();
        fflush(trace_file);
    }
}

void do_free_locked(void* ptr) {
    int ainfo_idx = ptr_map_remove(ptr);
    if (ainfo_idx < 0)
        return;  // untracked pointer
    trace_printf("- %x\n", ainfo_idx);
}

// ---- Hooks (called by the interposers below) ----
void ht_alloc(void* ptr, size_t size) {
    if (!trace_file || !ptr || g_in_hook)
        return;
    g_in_hook = 1;
    void* frames[HT_MAX_DEPTH];
    ht_ipres res[HT_MAX_DEPTH];
    int n = ht_capture(frames, HT_MAX_DEPTH);
    int nres = ht_presolve(frames, n, res);
    pthread_mutex_lock(&g_lock);
    do_alloc_locked(ptr, size, frames, n, res, nres);
    pthread_mutex_unlock(&g_lock);
    ht_ipres_free(res, nres);
    g_in_hook = 0;
}

void ht_free(void* ptr) {
    if (!trace_file || !ptr || g_in_hook)
        return;
    g_in_hook = 1;
    pthread_mutex_lock(&g_lock);
    do_free_locked(ptr);
    pthread_mutex_unlock(&g_lock);
    g_in_hook = 0;
}

void ht_realloc(void* old_ptr, void* new_ptr, size_t size) {
    if (!trace_file || g_in_hook)
        return;
    if (!new_ptr && size != 0)
        return;  // realloc failure: old_ptr still live
    g_in_hook = 1;
    void* frames[HT_MAX_DEPTH];
    ht_ipres res[HT_MAX_DEPTH];
    int n = 0, nres = 0;
    if (new_ptr) {
        n = ht_capture(frames, HT_MAX_DEPTH);
        nres = ht_presolve(frames, n, res);
    }
    pthread_mutex_lock(&g_lock);
    if (old_ptr)
        do_free_locked(old_ptr);
    if (new_ptr)
        do_alloc_locked(new_ptr, size, frames, n, res, nres);
    pthread_mutex_unlock(&g_lock);
    ht_ipres_free(res, nres);
    g_in_hook = 0;
}

constexpr unsigned HT_TMPFS_MAGIC = 0x01021994;

void ht_mmap(void* ptr, size_t length, int flags, int fd) {
    if (!trace_file || !ptr || ptr == MAP_FAILED || !length || g_in_hook)
        return;
    // Residency filter: anonymous (by FLAG) + tmpfs/shm + device maps (/dev/mali* — pinned GPU
    // memory) count; disk-backed regular files are evictable page cache and are skipped.
    int track = 1;
    char tag[96] = "mmap(anonymous)";
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) != 0) {
            track = 0;
        } else if (S_ISREG(st.st_mode)) {
            struct statfs sfs;
            if (fstatfs(fd, &sfs) != 0 || (unsigned)sfs.f_type != HT_TMPFS_MAGIC)
                track = 0;
        }
        if (track) {
            char link[48], path[80];
            snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
            ssize_t n = readlink(link, path, sizeof(path) - 1);
            if (n > 0) {
                path[n] = 0;
                snprintf(tag, sizeof(tag), "mmap(%s)", path);
            } else {
                snprintf(tag, sizeof(tag), "mmap(fd)");
            }
        }
    }
    if (!track && !(flags & MAP_FIXED))
        return;
    g_in_hook = 1;
    void* frames[HT_MAX_DEPTH];
    ht_ipres res[HT_MAX_DEPTH];
    int n = 0, nres = 0;
    if (track) {
        n = ht_capture(frames, HT_MAX_DEPTH);
        nres = ht_presolve(frames, n, res);
    }
    pthread_mutex_lock(&g_lock);
    if (flags & MAP_FIXED)
        do_munmap_locked(ptr, length);  // MAP_FIXED replaces overlapping tracked ranges
    if (track)
        do_mmap_locked(ptr, length, tag, frames, n, res, nres);
    pthread_mutex_unlock(&g_lock);
    ht_ipres_free(res, nres);
    g_in_hook = 0;
}

void ht_munmap(void* ptr, size_t length) {
    if (!trace_file || !ptr || !length || g_in_hook)
        return;
    g_in_hook = 1;
    pthread_mutex_lock(&g_lock);
    do_munmap_locked(ptr, length);
    pthread_mutex_unlock(&g_lock);
    g_in_hook = 0;
}

// ==== Process-wide host-library interposition ====
// dusklight's exe is first in the global symbol scope, so defining these here captures every
// allocation/mapping from libmali, mesa, SDL, libc-external callers, and dusklight itself. Real
// operation forwards to glibc (resolved once via dlsym(RTLD_NEXT)); zero recording when trace_file
// is null. glibc-INTERNAL mallocs/mmaps use internal aliases and bypass this (no double count).

void* (*r_malloc)(size_t);
void* (*r_calloc)(size_t, size_t);
void* (*r_realloc)(void*, size_t);
void (*r_free)(void*);
void* (*r_memalign)(size_t, size_t);
int (*r_posix_memalign)(void**, size_t, size_t);
void* (*r_aligned_alloc)(size_t, size_t);

// dlopen/dlclose ABBA guard: the tracer takes g_lock then symbolicates (dladdr -> loader lock);
// dlopen HOLDS the loader lock across its internal mallocs + constructors. Suppress feeds on a
// dlopen thread so it never wants g_lock while holding the loader lock (allocations still forward).
__thread int g_in_dl;
void* (*r_dlopen)(const char*, int);
int (*r_dlclose)(void*);

char hboot_buf[65536] __attribute__((aligned(16)));
size_t hboot_off;
// MUST be volatile + thread-local: hresolve() calls dlsym(), whose glibc internals calloc() (for
// __libc_dlerror_result), re-entering our calloc() on the same thread. A non-volatile latch can be
// kept in a register across the opaque dlsym() and the re-entrant calloc reads 0 -> infinite
// recursion / stack overflow (observed on device). volatile forces a real load/store.
__thread volatile int hresolving;

int hboot_owns(const void* p) {
    return (const char*)p >= hboot_buf && (const char*)p < hboot_buf + sizeof(hboot_buf);
}
void* hboot_alloc(size_t n) {
    size_t need = (n + 15) & ~(size_t)15;
    size_t off = __atomic_fetch_add(&hboot_off, need, __ATOMIC_RELAXED);
    if (off + need > sizeof(hboot_buf))
        return nullptr;
    return hboot_buf + off;  // bss: already zeroed
}

void hresolve() {
    if (r_free)
        return;
    hresolving = 1;
    r_malloc = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    r_calloc = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    r_realloc = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
    r_memalign = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "memalign");
    r_posix_memalign = (int (*)(void**, size_t, size_t))dlsym(RTLD_NEXT, "posix_memalign");
    r_aligned_alloc = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "aligned_alloc");
    hresolving = 0;
    // r_free last: it doubles as the "resolved" flag for the fast path.
    __atomic_store_n(&r_free, (void (*)(void*))dlsym(RTLD_NEXT, "free"), __ATOMIC_RELEASE);
}

}  // namespace

extern "C" {

void* malloc(size_t size) {
    if (!r_free) {
        if (hresolving)
            return hboot_alloc(size);
        hresolve();
    }
    void* p = r_malloc(size);
    if (p && !g_in_dl)
        ht_alloc(p, size);
    return p;
}

void* calloc(size_t nmemb, size_t size) {
    if (!r_free) {
        if (hresolving)
            return hboot_alloc(nmemb * size);
        hresolve();
    }
    void* p = r_calloc(nmemb, size);
    if (p && !g_in_dl)
        ht_alloc(p, nmemb * size);
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!r_free) {
        if (hresolving)
            return hboot_alloc(size);
        hresolve();
    }
    if (hboot_owns(ptr)) {  // migrate out of the bootstrap arena
        void* p = r_malloc(size);
        if (p) {
            size_t avail = (size_t)(hboot_buf + sizeof(hboot_buf) - (char*)ptr);
            memcpy(p, ptr, size < avail ? size : avail);
            if (!g_in_dl)
                ht_alloc(p, size);
        }
        return p;
    }
    void* np = r_realloc(ptr, size);
    if (!g_in_dl)
        ht_realloc(ptr, np, size);  // self-guards NULL/size==0
    return np;
}

void free(void* ptr) {
    if (!ptr || hboot_owns(ptr))
        return;
    if (!r_free)
        hresolve();
    if (!g_in_dl)
        ht_free(ptr);
    r_free(ptr);
}

void* memalign(size_t alignment, size_t size) {
    if (!r_free)
        hresolve();
    void* p = r_memalign ? r_memalign(alignment, size) : nullptr;
    if (p && !g_in_dl)
        ht_alloc(p, size);
    return p;
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (!r_free)
        hresolve();
    if (!r_posix_memalign)
        return 12;  // ENOMEM
    int rc = r_posix_memalign(memptr, alignment, size);
    if (rc == 0 && *memptr && !g_in_dl)
        ht_alloc(*memptr, size);
    return rc;
}

void* aligned_alloc(size_t alignment, size_t size) {
    if (!r_free)
        hresolve();
    void* p = r_aligned_alloc ? r_aligned_alloc(alignment, size) : nullptr;
    if (p && !g_in_dl)
        ht_alloc(p, size);
    return p;
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    void* ret = (void*)syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
    if (ret != MAP_FAILED)
        ht_mmap(ret, length, flags, fd);
    return ret;
}

// LFS-built libraries (libmali, mesa's libgallium) import mmap64, not mmap. On aarch64 off_t is
// 64-bit and glibc's mmap64 IS mmap; this unversioned exe definition satisfies the versioned ref.
void* mmap64(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    return mmap(addr, length, prot, flags, fd, offset);
}

int munmap(void* addr, size_t length) {
    int ret = (int)syscall(SYS_munmap, addr, length);
    if (ret == 0)
        ht_munmap(addr, length);
    return ret;
}

void* dlopen(const char* file, int mode) {
    if (!r_dlopen)
        r_dlopen = (void* (*)(const char*, int))dlsym(RTLD_NEXT, "dlopen");
    g_in_dl++;
    void* h = r_dlopen(file, mode);
    g_in_dl--;
    return h;
}

int dlclose(void* handle) {
    if (!r_dlclose)
        r_dlclose = (int (*)(void*))dlsym(RTLD_NEXT, "dlclose");
    g_in_dl++;
    int rc = r_dlclose(handle);
    g_in_dl--;
    return rc;
}

}  // extern "C"

#include "dusk/logging.h"

namespace dusk::heaptrack {

// Registered via atexit() in init(): closes the trace as LATE as possible, so session-lifetime
// allocations that are freed during the game's teardown and C++ static destruction get recorded as
// FREED rather than counted as leaks. Closing at main01-return (where shutdown() fires) is too
// early — the ModLoader singleton, the disc FST, aurora's GPU pools, etc. are all released after
// that point, and mis-booking every one of those frees as a leak is exactly what pushed the
// reported std::filesystem::path "leak" past peak RSS. A hard kill skips atexit, but the periodic
// fflush in the alloc hook leaves a valid prefix, so this only ever adds accuracy.
static void ht_atexit_finalize() {
    if (!trace_file)
        return;
    g_in_hook = 1;
    pthread_mutex_lock(&g_lock);
    g_active = 0;
    trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
    trace_flush();
    fclose(trace_file);
    trace_file = nullptr;
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "[dusk] heaptrack: libunwind guard fired %lu times (frame-pointer fallback)\n",
            g_unwind_faults);
    g_in_hook = 0;
}

void init() {
    const char* filename = std::getenv("DUSKLIGHT_HEAPTRACK");
    if (!filename || !*filename)
        return;
    if (g_active)
        return;

    g_in_hook = 1;  // this function's own allocations route through the interposers; don't trace them

    trace_file = fopen(filename, "w");
    if (!trace_file) {
        DuskLog.warn("heaptrack: failed to open '{}' for writing; tracing disabled", filename);
        g_in_hook = 0;
        return;
    }
    trace_buf = (char*)malloc(TRACE_BUF_SIZE);
    trace_buf_pos = 0;
    clock_gettime(CLOCK_MONOTONIC, &trace_start_time);

    // Close the trace at process exit, not at main01-return, so teardown/static-dtor frees land in
    // the recording. (Registered here under g_in_hook so atexit's internal bookkeeping alloc isn't
    // traced.) See ht_atexit_finalize.
    atexit(&ht_atexit_finalize);

    // Arm the SIGSEGV/SIGBUS guard so ht_capture can use libunwind (DWARF/CFI) as its primary walk.
    ht_install_unwind_guard();

    build_elf_index();  // dusklight .symtab -> good symbolication of the game's own (static) fns

    trace_printf("v 10400 1\n");  // heaptrack 1.4.0, file format v1
    char exe[1024];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = 0;
        trace_printf("x %lx %s\n", (unsigned long)len, exe);
    } else {
        trace_printf("x 9 dusklight\n");
    }
    trace_printf("I %lx %lx\n", (unsigned long)sysconf(_SC_PAGESIZE), (unsigned long)sysconf(_SC_PHYS_PAGES));
    trace_printf("c 0\n");

    g_active = 1;
    trace_file = trace_file;  // publish
    g_in_hook = 0;

    // Canary: prove the exe interposers feed the tracer (one matched +/- temporary allocation).
    void* canary = malloc(32);
    __asm__ volatile("" : : "r"(canary) : "memory");
    free(canary);

    g_in_hook = 1;
    pthread_mutex_lock(&g_lock);
    trace_flush();
    fflush(trace_file);
    pthread_mutex_unlock(&g_lock);
    g_in_hook = 0;

    DuskLog.info("heaptrack: tracing allocations (frame-pointer walk) to '{}'. View: heaptrack_gui '{}'",
                 filename, filename);
}

// NOT a close — a CHECKPOINT flush. Fires at main01-return (the start of the game's teardown) to
// guarantee everything up to that point is durably on disk even if a later teardown stage hard-exits
// before atexit runs. Recording deliberately CONTINUES through teardown; the file is closed by
// ht_atexit_finalize() at process exit so those session-lifetime frees are booked as freed, not
// leaked. (Idempotent and cheap; safe if atexit already fired.)
void shutdown() {
    if (!trace_file)
        return;
    g_in_hook = 1;
    pthread_mutex_lock(&g_lock);
    trace_printf("c %lx\n", (unsigned long)trace_elapsed_ms());
    trace_flush();
    fflush(trace_file);
    pthread_mutex_unlock(&g_lock);
    g_in_hook = 0;
}

}  // namespace dusk::heaptrack

#endif  // DUSK_HEAPTRACK_BUILD && !_WIN32
