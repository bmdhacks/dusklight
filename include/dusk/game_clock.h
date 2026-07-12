#pragma once

namespace dusk::game_clock {

void ensure_initialized();
void reset_frame_timer();

constexpr float sim_pace() { return 1.0f / 30.0f; }
constexpr float period_for_original_frames(float frame_count) { return frame_count * sim_pace(); }
constexpr float ui_maximum_dt() { return 0.05f; }
constexpr float ui_initial_dt() { return 1.0f / 60.0f; }

struct MainLoopPacer {
    float presentation_dt_seconds;
    bool is_interpolating;
    bool is_decoupled;
    int sim_ticks_to_run;
    float sim_pace;
};

// True when the sim is paced against the wall clock and the painter is hoisted out of
// fpcM_Management, so a slow render rate no longer slows the game down. Mutually exclusive with
// frame interpolation, which drives the opposite arrangement.
bool decoupled_active();

// Advances the state machines that live inside mDoGph_Painter (fade, wipe, menu particles). The
// painter runs once per presented frame, so in decoupled mode these must be stepped here instead,
// once per sim tick, or they run at the render rate.
void advance_paint_side_calcs();

MainLoopPacer advance_main_loop();
void commit_sim_tick();
float sample_interpolation_step();

float consume_interval(const void* consumer);

} // namespace dusk::game_clock
