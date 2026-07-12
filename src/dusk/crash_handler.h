#pragma once

namespace dusk::crash_handler {

void install();

// True once a SIGTERM/SIGINT has requested shutdown. The main loop polls this to
// break cleanly; a watchdog (see crash_handler.cpp) forces process exit if that
// cooperative shutdown wedges, so a stuck frame can't leave the launcher hanging.
bool termination_requested() noexcept;

}  // namespace dusk::crash_handler
