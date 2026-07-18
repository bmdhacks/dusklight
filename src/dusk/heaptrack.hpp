#ifndef DUSK_HEAPTRACK_HPP
#define DUSK_HEAPTRACK_HPP

// Dev-only, env-var-gated heap-allocation tracer for hunting OOM/leaks in dusklight.
//
// Emits the heaptrack v1 interpreted text format (open in heaptrack_gui / heaptrack_print), but
// does its OWN frame-pointer stack walk (x29 chain) instead of libunwind — upstream heaptrack's
// libunwind unwinder SIGSEGVs walking aurora's GL/driver stacks (verified: it crashes the instant
// it unwinds a gameplay allocation). This is a native-ELF port of machismo's proven heaptrack_trace
// tracer (frame-pointer walk + dladdr/.symtab symbolication + RESIDENT-relevant mmap tracking incl.
// /dev/mali* GPU memory + a periodic RSS ground-truth curve).
//
// Enable at runtime with DUSKLIGHT_HEAPTRACK=<path>; view the result with:
//   heaptrack_gui <path>       (or: heaptrack_print <path> | less)
//
// The process-wide malloc/mmap interposers that FEED the tracer are compiled only in a profiling
// build (cmake -DDUSK_HEAPTRACK_BUILD=ON, which also adds -fno-omit-frame-pointer). A normal build
// defines none of them (zero overhead); init()/shutdown() then no-op. Off by default.

namespace dusk::heaptrack {

// If DUSKLIGHT_HEAPTRACK=<path> is set, open the trace and begin recording. Call once, early
// (game_main entry). No-op if the env var is unset or this isn't a DUSK_HEAPTRACK_BUILD binary.
// Also arms an atexit() hook that closes the trace at process exit, so allocations freed during
// teardown/static destruction are recorded as freed rather than mis-counted as leaks.
void init();

// Checkpoint flush at the start of teardown (main01-return) — NOT a close. Recording continues
// through teardown; the trace is closed by the atexit hook at process exit. Call at shutdown; safe
// to call unconditionally.
void shutdown();

}  // namespace dusk::heaptrack

#endif  // DUSK_HEAPTRACK_HPP
