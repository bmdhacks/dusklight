#pragma once

// Fork-only: the mod WebGPU gfx service is disabled on this branch.
//
// Upstream's mods/svc/gfx.h hands mods raw wgpu* handles (WGPUDevice, WGPURenderPassEncoder,
// ...) and so requires <webgpu/webgpu.h>. This fork renders through a hand-rolled GLES backend
// with Dawn deleted, so those handles do not exist -- there is nothing to hand a mod, and no
// header to compile the declarations against. The service implementation
// (src/dusk/mods/svc/gfx.cpp) is dropped from the build and its module is not registered.
//
// The stage enum and a no-op gfx_run_stage are kept so the render-path call sites in
// m_Do_graphic.cpp stay byte-identical to upstream. Restore the include above if the mod SDK
// ever grows a GLES path.

class view_class;
class view_port_class;

typedef enum GfxStage {
    GFX_STAGE_SCENE_AFTER_TERRAIN = 0,
    GFX_STAGE_FRAME_BEFORE_HUD = 1,
    GFX_STAGE_FRAME_AFTER_HUD = 2,
    GFX_STAGE_SCENE_BEGIN = 3,
    GFX_STAGE_SCENE_AFTER_OPAQUE = 4,
} GfxStage;

namespace dusk::mods {

inline void gfx_run_stage(GfxStage /* stage */, const view_class* /* gameView */ = nullptr,
    const view_port_class* /* gameViewport */ = nullptr) {}

}  // namespace dusk::mods
