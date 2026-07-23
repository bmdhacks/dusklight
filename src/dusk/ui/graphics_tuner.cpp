#include "graphics_tuner.hpp"

#include "Z2AudioLib/Z2SeMgr.h"
#include "m_Do/m_Do_audio.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <fmt/format.h>

#include "aurora/lib/window.hpp"

#include "dusk/config.hpp"
#include "dusk/settings.h"
#include "dusk/texture_replacements.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

namespace dusk::ui {
namespace {

// Internal Resolution is a fixed ladder of *screen* fractions: the game renders at
// (physical drawable / N) and is upscaled to fill the panel. When the displayed area divides
// evenly by N each rendered pixel lands on an exact NxN block of panel pixels -- pixel-perfect,
// no upscale shimmer; whether that holds depends on the panel (and its 4:3 letterbox), so it is
// computed live in internal_res_upscale_factor rather than assumed per step. Ordered low->high;
// Auto (full screen) is the top. Auto's scale sentinel 0.f means "match the panel exactly".
struct InternalResStep {
    float scale;
    const char* label;
};
constexpr InternalResStep kInternalResSteps[] = {
    {1.0f / 3.0f, "1/3"},  // floor -- for very weak GPUs / large panels
    {0.5f, "1/2"},
    {2.0f / 3.0f, "2/3"},
    {0.75f, "3/4"},
    {0.0f, "Auto"},        // full screen, 1:1
};
constexpr int kInternalResStepCount = static_cast<int>(std::size(kInternalResSteps));
constexpr int kInternalResAutoIndex = kInternalResStepCount - 1;

float internal_res_index_to_scale(int index) {
    return kInternalResSteps[std::clamp(index, 0, kInternalResAutoIndex)].scale;
}

int internal_res_scale_to_index(float scale) {
    if (scale <= 0.001f || scale >= 0.9f) {
        return kInternalResAutoIndex;  // 0 (Auto) or a legacy >= screen value -> full screen
    }
    int best = 0;
    float bestErr = std::abs(kInternalResSteps[0].scale - scale);
    for (int i = 1; i < kInternalResAutoIndex; ++i) {  // skip Auto's 0.f sentinel
        const float err = std::abs(kInternalResSteps[i].scale - scale);
        if (err < bestErr) {
            bestErr = err;
            best = i;
        }
    }
    return best;
}

// If the current render size upscales to the displayed area by a whole number (equal in both
// axes), return that factor (1 = native, 2 = 2:1, ...); otherwise 0 (not pixel-perfect). The
// displayed area is the 4:3 letterbox of the panel when aspect is locked (TP renders 608x456 ==
// 4:3), else the full panel -- so a 16:9 1280x720 at 1/3 correctly reads 3:1 (960x720 / 320x240).
int internal_res_upscale_factor(u32 renderW, u32 renderH) {
    if (renderW == 0 || renderH == 0) {
        return 0;
    }
    const auto win = aurora::window::get_window_size();
    u32 regionW = win.native_fb_width;
    u32 regionH = win.native_fb_height;
    if (regionW == 0 || regionH == 0) {
        return 0;
    }
    if (getSettings().video.lockAspectRatio.getValue()) {
        constexpr float kGameAspect = 4.0f / 3.0f;
        if (static_cast<float>(regionW) / static_cast<float>(regionH) > kGameAspect) {
            regionW = static_cast<u32>(std::lround(static_cast<float>(regionH) * kGameAspect));
        } else {
            regionH = static_cast<u32>(std::lround(static_cast<float>(regionW) / kGameAspect));
        }
    }
    if (regionW % renderW != 0 || regionH % renderH != 0) {
        return 0;
    }
    const u32 fx = regionW / renderW;
    const u32 fy = regionH / renderH;
    return fx == fy ? static_cast<int>(fx) : 0;
}

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/tuner.rcss" />
</head>
<body>
    <div id="root" class="tuner-root">
        <div class="tuner">
            <div class="header">
                <div id="title"></div>
                <div id="carousel-container" class="carousel-container"></div>
            </div>
            <div id="description" class="description"></div>
            <div class="divider"></div>
            <div id="footer" class="footer"></div>
        </div>
    </div>
</body>
</rml>
)RML";

int get_value(GraphicsOption option) {
    switch (option) {
    case GraphicsOption::InternalResolution:
        // Float cvar -> fixed carousel index (0 = Auto, 1 = 0.5x .. 7 = 2.0x, see helpers above).
        return graphics_float_carousel_units(
            option, getSettings().game.internalResolutionScale.getValue());
    case GraphicsOption::ShadowResolution:
        return getSettings().game.shadowResolutionMultiplier.getValue();
    case GraphicsOption::Resampler:
        return static_cast<int>(getSettings().game.resampler.getValue());
    case GraphicsOption::BloomMode:
        return static_cast<int>(getSettings().game.bloomMode.getValue());
    case GraphicsOption::BloomMultiplier:
        return std::clamp(
            static_cast<int>(getSettings().game.bloomMultiplier.getValue() * 100.0f + 0.5f), 0,
            100);
    case GraphicsOption::DepthOfFieldMode:
        return static_cast<int>(getSettings().game.depthOfFieldMode.getValue());
    case GraphicsOption::TextureReplacements:
        return getSettings().game.enableTextureReplacements.getValue();
    }
    return 0;
}

void set_value(GraphicsOption option, int value) {
    switch (option) {
    case GraphicsOption::InternalResolution: {
        // Carousel index -> GameCube-native scale (0 = Auto, 1..7 = 0.5x..2.0x in fourths).
        const float scale = internal_res_index_to_scale(value);
        getSettings().game.internalResolutionScale.setValue(scale);
        VISetFrameBufferScale(scale);
        break;
    }
    case GraphicsOption::ShadowResolution:
        getSettings().game.shadowResolutionMultiplier.setValue(value);
        break;
    case GraphicsOption::Resampler: {
        const auto sampler = static_cast<Resampler>(std::clamp(value,
            static_cast<int>(Resampler::Bilinear),
            static_cast<int>(Resampler::Area)));
        getSettings().game.resampler.setValue(sampler);
        switch (sampler) {
        case Resampler::Area:
            aurora_set_resampler(SAMPLER_AREA);
            break;
        case Resampler::Bilinear:
        default:
            aurora_set_resampler(SAMPLER_BILINEAR);
            break;
        }
        break;
    }
    case GraphicsOption::BloomMode:
        getSettings().game.bloomMode.setValue(static_cast<BloomMode>(std::clamp(
            value, static_cast<int>(BloomMode::Off), static_cast<int>(BloomMode::Dusk))));
        break;
    case GraphicsOption::DepthOfFieldMode:
        getSettings().game.depthOfFieldMode.setValue(static_cast<DepthOfFieldMode>(std::clamp(
            value, static_cast<int>(DepthOfFieldMode::Off), static_cast<int>(DepthOfFieldMode::Dusk))));
        break;
    case GraphicsOption::BloomMultiplier:
        getSettings().game.bloomMultiplier.setValue(std::clamp(value, 0, 100) / 100.0f);
        break;
    case GraphicsOption::TextureReplacements:
        texture_replacements::set_enabled(static_cast<bool>(value));
        break;
    }
}

Rml::Element* create_stepped_carousel_root(Rml::Element* parent) {
    auto* doc = parent->GetOwnerDocument();
    auto root = doc->CreateElement("div");
    root->SetClass("stepped-carousel", true);
    root->SetAttribute("tabindex", "0");
    return parent->AppendChild(std::move(root));
}

Rml::Element* create_stepped_carousel_arrow(
    Rml::Element* parent, const Rml::String& className, const Rml::String& label) {
    auto* doc = parent->GetOwnerDocument();
    auto button = doc->CreateElement("button");
    button->SetClass("stepped-carousel-arrow", true);
    button->SetClass(className, true);
    button->SetInnerRML(label);
    return parent->AppendChild(std::move(button));
}

void update_carousel_arrow_color(Rml::Element* arrow, bool dim) {
    const Rml::Colourb& color = Rml::Colourb(255, 255, 255, dim ? 128 : 255);
    arrow->SetProperty(Rml::PropertyId::Color, Rml::Property(color, Rml::Unit::COLOUR));
}

}  // namespace

SteppedCarousel::SteppedCarousel(Rml::Element* parent, Props props)
    : Component(create_stepped_carousel_root(parent)), mProps(std::move(props)) {
    mPrevElem = create_stepped_carousel_arrow(mRoot, "prev", "&#xe5cb;");
    mValueElem = append(mRoot, "div");
    mValueElem->SetClass("stepped-carousel-value", true);
    mNextElem = create_stepped_carousel_arrow(mRoot, "next", "&#xe5cc;");

    listen(mPrevElem, Rml::EventId::Click,
        [this](Rml::Event&) { handle_nav_command(NavCommand::Left); });
    listen(mNextElem, Rml::EventId::Click,
        [this](Rml::Event&) { handle_nav_command(NavCommand::Right); });
    listen(mRoot, Rml::EventId::Keydown, [this](Rml::Event& event) {
        const auto cmd = map_nav_event(event);
        if (cmd != NavCommand::None && handle_nav_command(cmd)) {
            event.StopPropagation();
        }
    });
}

bool SteppedCarousel::focus() {
    return Component::focus();
}

void SteppedCarousel::update() {
    if (mValueElem == nullptr) {
        return;
    }
    const int value = std::clamp(mProps.getValue ? mProps.getValue() : 0, mProps.min, mProps.max);
    if (mProps.formatValue) {
        mValueElem->SetInnerRML(mProps.formatValue(value));
    } else {
        mValueElem->SetInnerRML(std::to_string(value));
    }

    update_carousel_arrow_color(mPrevElem, value == mProps.min);
    update_carousel_arrow_color(mNextElem, value == mProps.max);
}

bool SteppedCarousel::handle_nav_command(NavCommand cmd) {
    if (cmd == NavCommand::Left) {
        const int value = mProps.getValue ? mProps.getValue() : 0;
        apply(std::clamp(value - mProps.step, mProps.min, mProps.max));
        return true;
    }
    if (cmd == NavCommand::Right) {
        const int value = mProps.getValue ? mProps.getValue() : 0;
        apply(std::clamp(value + mProps.step, mProps.min, mProps.max));
        return true;
    }
    return false;
}

void SteppedCarousel::apply(int value) {
    const int nextValue = std::clamp(value, mProps.min, mProps.max);
    const int currentValue =
        std::clamp(mProps.getValue ? mProps.getValue() : 0, mProps.min, mProps.max);
    if (nextValue == currentValue) {
        return;
    }
    mDoAud_seStartMenu(kSoundItemChange);
    if (mProps.onChange) {
        mProps.onChange(nextValue);
    }
}

Rml::String format_graphics_setting_value(GraphicsOption option, int value) {
    switch (option) {
    case GraphicsOption::InternalResolution: {
        u32 width = 0;
        u32 height = 0;
        AuroraGetRenderSize(&width, &height);
        const InternalResStep& step = kInternalResSteps[std::clamp(value, 0, kInternalResAutoIndex)];
        // Show "N:1" when this size upscales to the display by a whole number (pixel-perfect).
        const int factor = internal_res_upscale_factor(width, height);
        if (factor > 0) {
            return fmt::format("{} ({}×{}, {}:1)", step.label, width, height, factor);
        }
        return fmt::format("{} ({}×{})", step.label, width, height);
    }
    case GraphicsOption::ShadowResolution:
        return fmt::format("{}×", value);
    case GraphicsOption::Resampler:
        switch (static_cast<Resampler>(value)) {
        case Resampler::Bilinear:
            return "Bilinear";
        case Resampler::Area:
            return "Area";
        }
        break;
    case GraphicsOption::BloomMode:
        switch (static_cast<BloomMode>(value)) {
        case BloomMode::Off:
            return "Off";
        case BloomMode::Classic:
            return "Classic";
        case BloomMode::Dusk:
            return "Dusklight";
        }
        break;
    case GraphicsOption::DepthOfFieldMode:
        switch (static_cast<DepthOfFieldMode>(value)) {
        case DepthOfFieldMode::Off:
            return "Off";
        case DepthOfFieldMode::Classic:
            return "Classic";
        case DepthOfFieldMode::Dusk:
            return "Dusklight";
        }
        break;
    case GraphicsOption::BloomMultiplier:
        return fmt::format("{}%", value);
    case GraphicsOption::TextureReplacements:
        return static_cast<bool>(value) ? "On" : "Off";
    }
    return "";
}

int graphics_float_carousel_units(GraphicsOption option, float rawValue) {
    switch (option) {
    case GraphicsOption::InternalResolution:
        // Fixed ladder: 0 = Auto, 1 = 0.5x, 2 = 0.75x, 3 = 1.0x (GameCube), ... 7 = 2.0x.
        return internal_res_scale_to_index(rawValue);
    case GraphicsOption::BloomMultiplier:
        return std::clamp(static_cast<int>(rawValue * 100.0f + 0.5f), 0, 100);
    default:
        return static_cast<int>(rawValue + 0.5f);
    }
}

GraphicsTuner::GraphicsTuner(GraphicsTunerProps props)
    : Document(kDocumentSource, false, DocumentScope::GraphicsTuner), mOption(props.option),
      mValueMin(props.valueMin), mValueMax(props.valueMax), mDefaultValue(props.defaultValue) {
    if (mDocument == nullptr) {
        return;
    }

    if (auto* title = mDocument->GetElementById("title")) {
        title->SetInnerRML(escape(props.title));
    }
    if (auto* description = mDocument->GetElementById("description")) {
        description->SetInnerRML(escape(props.helpText));
    }
    if (auto* carouselParent = mDocument->GetElementById("carousel-container")) {
        mCarousel = &add_component<SteppedCarousel>(carouselParent,
            SteppedCarousel::Props{
                .min = mValueMin,
                .max = mValueMax,
                .step = props.step,
                .getValue = [this] { return get_value(mOption); },
                .onChange = [this](int value) { set_value(mOption, value); },
                .formatValue =
                    [this](int value) { return format_graphics_setting_value(mOption, value); },
            });
    }

    if (auto* footer = mDocument->GetElementById("footer")) {
        auto& returnButton = add_component<Button>(footer, "\xE2\x86\x90 Return", "footer-button")
                                 .on_pressed([this] { pop(); });
        returnButton.root()->SetClass("return", true);
        auto& resetButton =
            add_component<Button>(footer, "Reset to default", "footer-button").on_pressed([this] {
                mDoAud_seStartMenu(kSoundItemChange);
                reset_default();
            });
        resetButton.root()->SetClass("reset", true);
    }

    // Hide document after transition completion
    mRoot = mDocument->GetElementById("root");
    listen(mRoot, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mRoot && !mRoot->HasAttribute("open") &&
            Document::visible())
        {
            Document::hide(mPendingClose);
        }
    });
}

void GraphicsTuner::show() {
    Document::show();
    mRoot->SetAttribute("open", "");
    mDoAud_seStartMenu(kSoundWindowOpen);
}

void GraphicsTuner::hide(bool close) {
    config::save();
    mRoot->RemoveAttribute("open");
    if (close) {
        mPendingClose = true;
        mDoAud_seStartMenu(kSoundWindowClose);
    }
}

void GraphicsTuner::update() {
    for (const auto& component : mComponents) {
        component->update();
    }
    Document::update();
}

bool GraphicsTuner::focus() {
    for (const auto& component : mComponents) {
        if (component->focus()) {
            return true;
        }
    }
    return false;
}

bool GraphicsTuner::visible() const {
    return mRoot->HasAttribute("open");
}

bool GraphicsTuner::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Cancel) {
        pop();
        return true;
    }

    if (mCarousel && mCarousel->handle_nav_command(cmd)) {
        return true;
    }

    return Document::handle_nav_command(event, cmd);
}

void GraphicsTuner::reset_default() {
    set_value(mOption, mDefaultValue);
}

}  // namespace dusk::ui
