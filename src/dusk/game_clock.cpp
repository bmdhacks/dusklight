#include "dusk/game_clock.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <dusk/frame_interpolation.h>
#include <dusk/logging.h>

namespace dusk::game_clock {

using clock = std::chrono::steady_clock;

bool s_initialized = false;
clock::time_point s_previous_sample{};
clock::time_point s_current_snapshot_time{};

std::unordered_map<uintptr_t, clock::time_point> s_interval_last_sample;

constexpr clock::duration kSimPeriodDuration =
    std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(sim_pace()));
constexpr clock::duration kAbnormalGapResetThreshold = std::chrono::milliseconds(250);
constexpr int kMaxSimTicksPerFrame = 3;

namespace {
// Long play sessions log to SD cards, so keep this sparse.
constexpr float kPacingReportPeriod = 30.0f;
int s_report_paints = 0;
int s_report_ticks = 0;
clock::time_point s_report_last{};

// The sim rate and the render rate are the same number in every mode but decoupled, where the whole
// point is that they diverge. Report both so a slow device can be told apart from a slow game.
void record_pacing(const MainLoopPacer& out) {
    ++s_report_paints;
    s_report_ticks += out.sim_ticks_to_run;

    const clock::time_point now = clock::now();
    if (s_report_last.time_since_epoch().count() == 0) {
        s_report_last = now;
        return;
    }
    const float elapsed = std::chrono::duration<float>(now - s_report_last).count();
    if (elapsed < kPacingReportPeriod) {
        return;
    }

    const char* mode = out.is_decoupled       ? "decoupled"
                       : out.is_interpolating ? "interpolating"
                                              : "vanilla";
    DuskLog.info("[clock] sim {:.1f}/s, paint {:.1f}/s ({})", static_cast<float>(s_report_ticks) / elapsed,
                 static_cast<float>(s_report_paints) / elapsed, mode);

    s_report_ticks = 0;
    s_report_paints = 0;
    s_report_last = now;
}
}  // namespace

void ensure_initialized() {
    if (s_initialized) {
        return;
    }
    s_previous_sample = clock::now();
    s_current_snapshot_time = s_previous_sample;
    s_initialized = true;
}

void reset_frame_timer() {
    s_previous_sample = clock::now();
    s_current_snapshot_time = s_previous_sample - kSimPeriodDuration;
}

bool decoupled_active() {
    return dusk::getSettings().video.decoupleSimFromRender.getValue() &&
           dusk::getSettings().game.enableFrameInterpolation.getValue() ==
               dusk::FrameInterpMode::Off;
}

MainLoopPacer advance_main_loop() {
    ensure_initialized();

    const clock::time_point now = clock::now();
    const clock::duration frame_gap = now - s_previous_sample;
    const float presentation_dt = std::chrono::duration<float>(frame_gap).count();
    s_previous_sample = now;

    MainLoopPacer out{};
    out.presentation_dt_seconds = presentation_dt;

    const bool should_interpolate = dusk::getSettings().game.enableFrameInterpolation.getValue() !=
                                        dusk::FrameInterpMode::Off &&
                                    !dusk::getTransientSettings().skipFrameRateLimit;
    const bool decoupled = !should_interpolate && decoupled_active();
    out.is_interpolating = should_interpolate;
    out.is_decoupled = decoupled;
    out.sim_pace = sim_pace();

    if (!should_interpolate && !decoupled) {
        s_current_snapshot_time = now;
        out.sim_ticks_to_run = 1;
        record_pacing(out);
        return out;
    }

    if (frame_gap > kAbnormalGapResetThreshold) {
        s_current_snapshot_time = now - kSimPeriodDuration;
        out.sim_ticks_to_run = decoupled ? 1 : 0;
        record_pacing(out);
        return out;
    }

    // Interpolation deliberately holds the sim one period behind `now`, so it always has a pair of
    // snapshots to interpolate between. Decoupled mode has nothing to interpolate, so it catches the
    // sim all the way up to the wall clock instead.
    const clock::time_point target = decoupled ? now : now - kSimPeriodDuration;

    int sim_ticks_to_run = 0;
    clock::time_point projected_snapshot_time = s_current_snapshot_time;
    while (sim_ticks_to_run < kMaxSimTicksPerFrame && projected_snapshot_time < target) {
        projected_snapshot_time += kSimPeriodDuration;
        sim_ticks_to_run++;
    }

    if (decoupled) {
        if (sim_ticks_to_run == 0) {
            // Ahead of the wall clock (fast hardware, or the turbo key). Advance one tick anyway,
            // and pull the snapshot back so it cannot drift arbitrarily far ahead of `now`.
            s_current_snapshot_time = now - kSimPeriodDuration;
            sim_ticks_to_run = 1;
        } else if (projected_snapshot_time < target) {
            // Hit the tick cap while still behind. Drop the outstanding debt rather than carrying
            // it, so a heavy scene runs slow but never sprints to catch up once it lightens.
            s_current_snapshot_time = now - kSimPeriodDuration * sim_ticks_to_run;
        }
    }

    out.sim_ticks_to_run = sim_ticks_to_run;
    record_pacing(out);
    return out;
}

void commit_sim_tick() {
    ensure_initialized();
    s_current_snapshot_time += kSimPeriodDuration;
}

float sample_interpolation_step() {
    ensure_initialized();
    const float step =
        std::chrono::duration<float>(clock::now() - s_current_snapshot_time).count() / sim_pace();
    return std::clamp(step, 0.0f, 1.0f);
}

float consume_interval(const void* consumer) {
    ensure_initialized();
    const uintptr_t key = reinterpret_cast<uintptr_t>(consumer);
    const clock::time_point now = clock::now();

    float dt = ui_initial_dt();
    const auto it = s_interval_last_sample.find(key);
    if (it != s_interval_last_sample.end()) {
        dt = std::chrono::duration<float>(now - it->second).count();
        dt = std::min(dt, ui_maximum_dt());
    }
    s_interval_last_sample[key] = now;
    return dt;
}

}  // namespace dusk::game_clock
