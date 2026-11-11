#include "effects.hpp"
#include "../render/graphics.hpp"
#include "../theme/theme.hpp"
#include "../ui/presentation.hpp"
#include "tween.hpp"
#include <GLES2/gl2.h>
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <render/backend/render_backend.h>
#include <render/core/texture_manager.h>
#include <render/text/cairo_text_renderer.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace blast {
namespace effects_detail {
constexpr float pi = 3.14159265359f;
constexpr std::size_t maxGlyphRuns = 128, maxGlyphs = 32;
constexpr unsigned noViewport = std::numeric_limits<unsigned>::max();
struct Rect {
    Vec2 min{}, max{};
};
struct Anchor {
    std::uint64_t buffer = 0, id = 0, cycle = 0, order = 0;
    unsigned viewport = noViewport;
    Rect rect;
};
enum class Shape { Dot, Spark, Fragment, Ring, Veil };
struct GlyphRun {
    std::uint64_t serial = 0;
    std::string text;
    TextFont font;
    Color tint;
    std::array<std::size_t, maxGlyphs + 1> offsets{};
    std::size_t count = 0, references = 0;
    float size = 14;
};
struct Particle {
    Vec2 pos{}, vel{};
    Color tint{};
    Rect rect{}, clip{}, anchorRect{};
    float age = 0, life = 1, size = 3, angle = 0, spin = 0, gravity = 1, drag = 1;
    Shape shape = Shape::Dot;
    unsigned viewport = noViewport, glyphRun = maxGlyphRuns, glyph = 0;
    std::uint64_t anchorBuffer = 0, anchorId = 0;
};
} // namespace effects_detail
using namespace effects_detail;
namespace {
bool valid(Rect r) {
    return std::isfinite(r.min.x) && std::isfinite(r.min.y) && std::isfinite(r.max.x) &&
           std::isfinite(r.max.y) && r.max.x > r.min.x && r.max.y > r.min.y;
}
Rect intersect(Rect a, Rect b) {
    return {{std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y)},
            {std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y)}};
}
// Fixed open-addressed registration table: no per-row allocation during native layout.
std::array<Anchor, 4096> anchors;
std::array<Rect, 128> viewports;
std::uint64_t cycle = 1, registrationOrder = 0;
std::size_t anchorCount = 0, viewportCount = 0;
unsigned currentViewport = noViewport;
std::uint64_t glyphSerial = 0;
std::uint64_t hash(std::string_view a, std::string_view b = {}) {
    std::uint64_t h = 1469598103934665603ULL;
    for (auto value : {a, b}) {
        for (unsigned char c : value) {
            if (c >= 'A' && c <= 'Z')
                c += 32;
            if (c == '[')
                c = '{';
            else if (c == ']')
                c = '}';
            else if (c == '\\')
                c = '|';
            else if (c == '^')
                c = '~';
            h = (h ^ c) * 1099511628211ULL;
        }
        h = (h ^ 0xff) * 1099511628211ULL;
    }
    return h;
}
std::uint64_t messageHash(std::string_view id) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : id)
        h = (h ^ c) * 1099511628211ULL;
    return h;
}
std::size_t anchorSlot(std::uint64_t buffer, std::uint64_t id, unsigned viewport) {
    return (buffer ^ (id + 0x9e3779b97f4a7c15ULL + (buffer << 6) + (buffer >> 2)) ^
            (std::uint64_t(viewport) * 0x9e3779b97f4a7c15ULL)) &
           (anchors.size() - 1);
}
const Anchor *findAnchor(std::uint64_t buffer, std::uint64_t id, unsigned viewport) {
    auto slot = anchorSlot(buffer, id, viewport);
    for (std::size_t i = 0; i < anchors.size(); ++i, slot = (slot + 1) % anchors.size()) {
        const auto &a = anchors[slot];
        if (a.cycle != cycle)
            return nullptr;
        if (a.buffer == buffer && a.id == id && a.viewport == viewport)
            return &a;
    }
    return nullptr;
}
void expireAnchors() {
    ++cycle;
    anchorCount = viewportCount = 0;
    currentViewport = noViewport;
}
Vec2 center(Rect r) { return {(r.min.x + r.max.x) * .5f, (r.min.y + r.max.y) * .5f}; }
Color color(Color c, float alpha = 1) {
    c.a = std::clamp(c.a * alpha, 0.f, 1.f);
    return c;
}
std::uint32_t randomState = 0xb1a57;
float random01() {
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return float(randomState & 0xffffff) / 16777215.f;
}
float random(float low, float high) { return low + (high - low) * random01(); }
enum class Effect { None, Kick, Split, Mention, React, Redact, Join, Part, Disconnect };
struct Directive {
    std::string_view event;
    Effect effect;
    const char *clip;
};
constexpr Directive director[] = {{"kick", Effect::Kick, "boom"},
                                  {"ban", Effect::Kick, "boom"},
                                  {"netsplit", Effect::Split, "rumble"},
                                  {"mention", Effect::Mention, "ding"},
                                  {"reaction", Effect::React, "pop"},
                                  {"redact", Effect::Redact, "hush"},
                                  {"join", Effect::Join, "blip"},
                                  {"part", Effect::Part, "whoosh"},
                                  {"quit", Effect::Part, "whoosh"},
                                  {"send", Effect::None, "click"},
                                  {"echo", Effect::None, "tick"},
                                  {"unread", Effect::None, "tick"},
                                  {"disconnect", Effect::Disconnect, "power_down"},
                                  {"message", Effect::None, "tick"},
                                  {"action", Effect::None, "tick"},
                                  {"notice", Effect::None, "tick"}};
constexpr const char *clipNames[] = {"boom",   "rumble", "ding", "blip", "tick",
                                     "whoosh", "click",  "pop",  "hush", "power_down"};
struct NativeState {
    GLint framebuffer = 0;
    std::array<GLint, 4> viewport{}, scissor{};
    bool clipped = false;
    NativeState() {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        clipped = glIsEnabled(GL_SCISSOR_TEST);
        if (clipped)
            glGetIntegerv(GL_SCISSOR_BOX, scissor.data());
    }
    void restore(RenderBackend &backend) const {
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        backend.setViewport(static_cast<std::uint32_t>(viewport[2]),
                            static_cast<std::uint32_t>(viewport[3]));
        if (clipped)
            backend.setScissor({scissor[0], scissor[1], scissor[2], scissor[3]});
        else
            backend.disableScissor();
    }
};
// Every context-bound object belongs to the backend, not to the Effects/model lifetime.
class ParticleResources final : public RenderExtension {
  public:
    struct RasterRun {
        std::uint64_t serial = 0;
        float scale = 0, width = 0, height = 0, left = 0;
        std::unique_ptr<RenderFramebuffer> framebuffer;
        std::array<TextCharSpan, maxGlyphs> spans{};
    };
    TextureHandle dot;
    std::array<RasterRun, maxGlyphRuns> runs;
    CairoTextRenderer text;
    TextureManager *textures = nullptr;
    bool initialized = false;
    std::vector<std::size_t> offsets;
    std::vector<TextCharSpan> spans;

    void initialize(RenderBackend &backend) {
        if (initialized)
            return;
        textures = &backend.textureManager();
        text.initialize(&backend, textures);
        offsets.reserve(maxGlyphs + 1);
        spans.reserve(maxGlyphs);
        std::array<std::uint8_t, 32 * 32> pixels{};
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const float dx = (x - 15.5f) / 15.5f, dy = (y - 15.5f) / 15.5f;
                const float a = std::pow(std::max(0.f, 1 - std::sqrt(dx * dx + dy * dy)), 2.f);
                pixels[y * 32 + x] = static_cast<std::uint8_t>(a * 255);
            }
        dot = textures->loadFromPixels(pixels.data(), 32, 32, TextureDataFormat::Alpha);
        initialized = true;
    }
    const RasterRun &rasterize(RenderBackend &backend, const GlyphRun &run, unsigned slot,
                               float scale) {
        auto &r = runs[slot];
        if (r.serial == run.serial && r.scale == scale && r.framebuffer)
            return r;
        const NativeState saved;
        const auto metrics = text.measure(scale, run.text, run.size, run.font);
        const float left = std::floor(std::min(0.f, metrics.inkLeft) * scale) / scale - 1 / scale;
        const float right =
            std::ceil(std::max(metrics.width, metrics.inkRight) * scale) / scale + 1 / scale;
        const float top =
            std::floor(std::min(metrics.top, metrics.inkTop) * scale) / scale - 1 / scale;
        const float bottom =
            std::ceil(std::max(metrics.bottom, metrics.inkBottom) * scale) / scale + 1 / scale;
        const auto width = static_cast<unsigned>(std::max(1.f, std::ceil((right - left) * scale)));
        const auto height = static_cast<unsigned>(std::max(1.f, std::ceil((bottom - top) * scale)));
        if (width > static_cast<unsigned>(backend.maxTextureSize()) ||
            height > static_cast<unsigned>(backend.maxTextureSize()))
            throw std::runtime_error("native kick glyph texture exceeds the renderer limit");
        if (!r.framebuffer || r.framebuffer->width() != width || r.framebuffer->height() != height)
            r.framebuffer = backend.createFramebuffer(width, height);
        if (!r.framebuffer || !r.framebuffer->valid())
            throw std::runtime_error("cannot allocate native kick glyph texture");
        r.width = float(width) / scale;
        r.height = float(height) / scale;
        r.left = left;
        offsets.assign(run.offsets.begin(), run.offsets.begin() + run.count + 1);
        text.measureCharSpans(scale, run.text, run.size, offsets, spans, run.font);
        for (std::size_t i = 0; i < run.count; ++i) {
            r.spans[i] = i < spans.size() ? spans[i] : TextCharSpan{};
            if (r.spans[i].left <= 0)
                r.spans[i].left = std::min(r.spans[i].left, metrics.inkLeft);
            if (r.spans[i].right >= metrics.width)
                r.spans[i].right = std::max(r.spans[i].right, metrics.inkRight);
        }
        backend.bindFramebuffer(*r.framebuffer);
        backend.setViewport(width, height);
        backend.disableScissor();
        backend.clear(rgba(0, 0, 0, 0));
        backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
        text.draw(scale, r.width, r.height, -left, -top, run.text, run.size, withAlpha(run.tint, 1),
                  Mat3::identity(), run.font);
        saved.restore(backend);
        r.serial = run.serial;
        r.scale = scale;
        return r;
    }
    void destroy() override {
        for (auto &r : runs)
            r = {};
        if (textures && dot.valid())
            textures->unload(dot);
        dot = {};
        text.cleanup();
        textures = nullptr;
        initialized = false;
    }
    void abandon() noexcept override {
        for (auto &r : runs) {
            if (r.framebuffer)
                r.framebuffer->abandon();
            r = {};
        }
        dot = {};
        text.abandonGlyphTextures();
        text.cleanup();
        textures = nullptr;
        initialized = false;
    }
};
void softQuad(RenderBackend &backend, const CustomDrawContext &context, TextureId texture, Vec2 pos,
              Vec2 half, float angle, Color tint) {
    backend.drawGlyph({.texture = texture,
                       .surfaceWidth = context.surfaceWidth,
                       .surfaceHeight = context.surfaceHeight,
                       .width = half.x * 2,
                       .height = half.y * 2,
                       .tint = tint,
                       .tinted = true,
                       .transform = Mat3::translation(pos.x, pos.y) * Mat3::rotation(angle) *
                                    Mat3::translation(-half.x, -half.y)});
}
void nativeRect(RenderBackend &backend, const CustomDrawContext &context, Vec2 pos, Vec2 half,
                float angle, Color tint, float radius, bool ring = false, float thickness = 0) {
    RoundedRectStyle style;
    style.fill = tint;
    style.radius = radius;
    if (ring) {
        style.fillMode = FillMode::None;
        style.border = tint;
        style.borderWidth = thickness;
    }
    backend.drawRect(context.surfaceWidth, context.surfaceHeight, half.x * 2, half.y * 2, style,
                     Mat3::translation(pos.x, pos.y) * Mat3::rotation(angle) *
                         Mat3::translation(-half.x, -half.y));
}
} // namespace

void fxViewport(float x, float y, float width, float height) {
    const Rect rect{{x, y}, {x + width, y + height}};
    currentViewport = noViewport;
    if (!valid(rect))
        return;
    for (std::size_t i = 0; i < viewportCount; ++i)
        if (viewports[i].min.x == x && viewports[i].min.y == y && viewports[i].max.x == x + width &&
            viewports[i].max.y == y + height) {
            currentViewport = static_cast<unsigned>(i);
            return;
        }
    if (viewportCount < viewports.size()) {
        currentViewport = static_cast<unsigned>(viewportCount);
        viewports[viewportCount++] = rect;
    }
}
void fxAnchor(std::string_view server, std::string_view target, std::string_view id, float x,
              float y, float width, float height) {
    if (currentViewport == noViewport)
        return;
    const Rect rect = intersect({{x, y}, {x + width, y + height}}, viewports[currentViewport]);
    if (!valid(rect))
        return;
    const auto buffer = hash(server, target), message = messageHash(id);
    auto slot = anchorSlot(buffer, message, currentViewport);
    for (std::size_t i = 0; i < anchors.size(); ++i, slot = (slot + 1) % anchors.size()) {
        auto &a = anchors[slot];
        if (a.cycle == cycle &&
            (a.buffer != buffer || a.id != message || a.viewport != currentViewport))
            continue;
        if (a.cycle != cycle && anchorCount == 2048)
            return;
        if (a.cycle != cycle)
            ++anchorCount;
        a = {buffer, message, cycle, ++registrationOrder, currentViewport, rect};
        return;
    }
}

struct Effects::Impl {
    struct Overlay final : CustomDrawNode {
        Impl &owner;
        explicit Overlay(Impl &impl) : owner(impl) {
            setParticipatesInLayout(false);
            setHitTestVisible(false);
            setExcludeSubtreeFromTabOrder(true);
            setZIndex(std::numeric_limits<std::int32_t>::max() - 1);
        }
        ~Overlay() override { owner.overlay = nullptr; }
        void draw(RenderBackend &backend, const CustomDrawContext &context) const override {
            drawVisualPostprocess(backend, context);
            owner.render(backend, context);
        }
    };
    Overlay *overlay = nullptr;
    std::array<Particle, 4096> particles{};
    std::array<GlyphRun, maxGlyphRuns> glyphRuns;
    std::size_t count = 0;
    MIX_Mixer *mixer = nullptr;
    std::array<MIX_Audio *, std::size(clipNames)> audio{};
    struct Voice {
        MIX_Track *track = nullptr;
        std::size_t directive = 0;
    };
    std::array<Voice, 24> voices;
    std::array<double, std::size(director)> lastCue;
    std::size_t voiceCursor = 0;
    bool audioInitialized = false, mixerInitialized = false;
    std::string audioError;
    double previous = -1, split = -100, shake = -100, disconnect = -100;
    std::uint64_t visualVersion = 0, themeVersion = 0;
    Vec2 jitter{};
    FxLevel level = FxLevel::Full;
    float fontSize = 14;
    TextFont font;
    Anchor emissionAnchor;
    Impl() {
        lastCue.fill(-100);
        // Audio failure is visible but never prevents connecting to IRC.
        audioInitialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (!audioInitialized) {
            audioError = SDL_GetError();
            return;
        }
        mixerInitialized = MIX_Init();
        if (!mixerInitialized) {
            audioError = SDL_GetError();
            return;
        }
        mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (!mixer) {
            audioError = SDL_GetError();
            return;
        }
        std::array<std::filesystem::path, 4> roots;
#ifdef BLASTIRC_ASSET_DIR
        roots[0] = BLASTIRC_ASSET_DIR;
#endif
        roots[1] = std::filesystem::path("assets");
        const char *base = SDL_GetBasePath();
        if (base) {
            roots[2] = std::filesystem::path(base) / "assets";
            roots[3] = std::filesystem::path(base) / "../share/blastirc/assets";
        }
        for (std::size_t i = 0; i < audio.size(); ++i) {
            for (const auto &root : roots) {
                if (root.empty())
                    continue;
                const auto path = root / "sfx" / (std::string(clipNames[i]) + ".ogg");
                std::error_code error;
                if (std::filesystem::is_regular_file(path, error)) {
                    audio[i] = MIX_LoadAudio(mixer, path.c_str(), true);
                    if (audio[i])
                        break;
                }
            }
            if (!audio[i])
                audioError = std::string("cannot load ") + clipNames[i] + ".ogg: " + SDL_GetError();
        }
        for (auto &v : voices) {
            v.track = MIX_CreateTrack(mixer);
            if (!v.track)
                audioError = SDL_GetError();
        }
    }
    ~Impl() {
        if (overlay && overlay->parent())
            overlay->parent()->removeChild(overlay);
        for (auto &v : voices)
            if (v.track)
                MIX_DestroyTrack(v.track);
        for (auto *a : audio)
            if (a)
                MIX_DestroyAudio(a);
        if (mixer)
            MIX_DestroyMixer(mixer);
        if (mixerInitialized)
            MIX_Quit();
        if (audioInitialized)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    void remove(std::size_t i) {
        const auto run = particles[i].glyphRun;
        if (run < glyphRuns.size() && glyphRuns[run].references)
            --glyphRuns[run].references;
        particles[i] = particles[--count];
    }
    void clearMotion() {
        count = 0;
        for (auto &run : glyphRuns)
            run.references = 0;
        split = shake = disconnect = -100;
        jitter = {};
        setVisualImpulse(0, 0, -100);
    }
    void stop() {
        clearMotion();
        for (auto &v : voices)
            if (v.track)
                MIX_StopTrack(v.track, 0);
        if (mixer)
            MIX_SetMixerGain(mixer, 0);
    }
    Cue cue(const Settings &s, const Directive &d) const {
        if (auto i = s.cues.find(std::string(d.event)); i != s.cues.end())
            return i->second;
        if (auto i = s.cues.find(d.clip); i != s.cues.end())
            return i->second;
        return {};
    }
    void sound(const Settings &s, const Directive &d, std::size_t index, double now) {
        if (!mixer || !s.sound)
            return;
        const Cue config = cue(s, d);
        if (!config.enabled || config.volume <= 0)
            return;
        // ponytail: the existing per-cue 45ms budget caps burst spam within 24 voices.
        if (now - lastCue[index] < .045)
            return;
        lastCue[index] = now;
        std::size_t clip = 0;
        while (clip < std::size(clipNames) && std::string_view(clipNames[clip]) != d.clip)
            ++clip;
        if (clip == audio.size() || !audio[clip])
            return;
        Voice *voice = nullptr;
        for (auto &candidate : voices)
            if (candidate.track && !MIX_TrackPlaying(candidate.track)) {
                voice = &candidate;
                break;
            }
        if (!voice)
            voice = &voices[voiceCursor++ % voices.size()];
        if (!voice->track)
            return;
        MIX_StopTrack(voice->track, 0);
        MIX_SetTrackAudio(voice->track, audio[clip]);
        float variation = 1;
        if (s.pitchVariation == "natural")
            variation = random(.96f, 1.04f);
        else if (s.pitchVariation == "musical") {
            constexpr float semitones[] = {-.12f, 0, .12f};
            variation = std::pow(2.f, semitones[static_cast<unsigned>(random01() * 2.999f)]);
        }
        MIX_SetTrackGain(voice->track, std::clamp(config.volume, 0.f, 1.f));
        MIX_SetTrackFrequencyRatio(voice->track, std::clamp(config.pitch * variation, .25f, 4.f));
        MIX_PlayTrack(voice->track, 0);
        voice->directive = index;
    }
    Particle *add(Shape shape, Vec2 position, Color tint, float life, float size) {
        if (count == particles.size())
            return nullptr;
        auto &p = particles[count++];
        p = {};
        p.shape = shape;
        p.pos = position;
        p.tint = tint;
        p.life = life;
        p.size = size;
        p.anchorBuffer = emissionAnchor.buffer;
        p.anchorId = emissionAnchor.id;
        p.viewport = emissionAnchor.viewport;
        p.anchorRect = emissionAnchor.rect;
        p.clip = viewports[p.viewport];
        return &p;
    }
    void burst(Vec2 pos, Color tint, int n, float speed, bool soft = false) {
        for (int i = 0; i < n; ++i)
            if (auto *p = add(soft ? Shape::Fragment : Shape::Spark, pos, tint,
                              soft ? random(.3f, .6f) : random(.25f, .5f), random(1.5f, 4.f))) {
                const float angle = random(-pi, pi), force = random(speed * .2f, speed);
                p->vel = {std::cos(angle) * force, std::sin(angle) * force};
                p->spin = random(-6, 6);
                p->angle = angle;
                p->gravity = soft ? .2f : 1.f;
                p->drag = soft ? 2.f : .8f;
            }
    }
    void ring(Vec2 pos, Color tint, float radius, float life) {
        if (auto *p = add(Shape::Ring, pos, tint, life, radius))
            p->gravity = 0;
    }
    void shatter(Vec2 pos, std::string_view subject, Color tint) {
        auto run = std::find_if(glyphRuns.begin(), glyphRuns.end(),
                                [](const GlyphRun &r) { return r.references == 0; });
        if (run == glyphRuns.end() || subject.empty())
            return;
        run->count = 0;
        std::size_t end = 0;
        while (end < subject.size() && run->count < maxGlyphs) {
            run->offsets[run->count++] = end++;
            while (end < subject.size() &&
                   (static_cast<unsigned char>(subject[end]) & 0xc0) == 0x80)
                ++end;
        }
        run->offsets[run->count] = end;
        run->text.assign(subject.substr(0, end));
        run->font = font;
        run->size = fontSize;
        run->tint = tint;
        run->serial = ++glyphSerial;
        const auto slot = static_cast<unsigned>(run - glyphRuns.begin());
        for (unsigned i = 0; i < run->count; ++i) {
            auto *p = add(Shape::Fragment, pos, tint, .3f, fontSize * .65f);
            if (!p)
                break;
            p->glyphRun = slot;
            p->glyph = i;
            ++run->references;
            p->vel = {random(-130, 130), random(-150, -25)};
            p->spin = random(-12, 12);
            p->gravity = 1;
            p->drag = .5f;
        }
    }
    Anchor anchor(const Event &event) const {
        const auto b = hash(event.server, event.target),
                   id = messageHash(event.id.empty() ? event.label : event.id);
        const bool exact = !event.id.empty() || !event.label.empty();
        Anchor latest{}, match{};
        for (const auto &a : anchors) {
            if (a.cycle != cycle || a.buffer != b)
                continue;
            if (a.order > latest.order)
                latest = a;
            if (exact && a.id == id && a.order > match.order)
                match = a;
        }
        // An identified offscreen message must not animate the latest unrelated row.
        return exact ? match : latest;
    }
    void event(const Event &e, const Directive &d, Model &model) {
        const bool full = model.settings.fx == FxLevel::Full;
        const Anchor anchored = anchor(e);
        const Rect row = anchored.rect;
        const Vec2 pos = center(row);
        const auto error = themeColor("error"), secondary = themeColor("secondary"),
                   tertiary = themeColor("tertiary");
        if (d.effect == Effect::Split) {
            if (full)
                split = model.now;
            return;
        }
        if (d.effect == Effect::Disconnect) {
            disconnect = model.now;
            return;
        }
        if (!valid(row) || anchored.viewport >= viewportCount)
            return;
        emissionAnchor = anchored;
        switch (d.effect) {
        case Effect::Kick: {
            ring(pos, error, full ? 140 : 42, .3f);
            burst(pos, error, full ? 128 : 12, full ? 250 : 60);
            const auto victim = e.tags.find("victim");
            const std::string_view subject =
                victim != e.tags.end() ? std::string_view(victim->second)
                                       : (e.params.size() > 1 ? std::string_view(e.params[1])
                                                              : std::string_view(e.nick));
            shatter({row.min.x + 12, pos.y}, subject, error);
            if (full)
                shake = model.now;
            break;
        }
        case Effect::Mention:
            if (auto *p = add(Shape::Veil, pos, tertiary, 3.3f, 1))
                p->rect = row;
            break;
        case Effect::React:
            ring(pos, tertiary, full ? 30 : 18, .25f);
            if (auto *p = add(Shape::Veil, pos, tertiary, .65f, 1))
                p->rect = row;
            if (full)
                burst(pos, tertiary, 8, 45);
            break;
        case Effect::Redact: {
            Color gray = themeColor("on_surface_variant");
            const float l = .2126f * gray.r + .7152f * gray.g + .0722f * gray.b;
            gray = {l, l, l, .6f};
            burst(pos, gray, full ? 38 : 8, 35, true);
            break;
        }
        case Effect::Join:
            ring({row.min.x + 8, pos.y}, secondary, full ? 18 : 10, .18f);
            break;
        case Effect::Part:
            if (full)
                burst({row.min.x + 8, pos.y}, secondary, 8, 20, true);
            break;
        default:
            break;
        }
    }
    void panelMotion(double now, bool full) {
        const float age = float(now - split), desatAge = float(now - disconnect);
        float distance = 0;
        if (full && age >= 0 && age < .85f) {
            if (age < .22f)
                distance = 200 * fx::cubicOut(age / .22f);
            else if (age < .52f)
                distance = 200;
            else
                distance = 200 * (1 - fx::cubicOut((age - .52f) / .16f));
        }
        const float desaturate = desatAge >= 0 && desatAge < .9f
                                     ? (desatAge < .2f ? fx::smooth(desatAge / .2f)
                                                       : 1 - fx::smooth((desatAge - .2f) / .7f)) *
                                           .8f
                                     : 0;
        setVisualImpulse(distance, desaturate, age);
    }
    void updateParticles(Model &model, float dt) {
        jitter = {};
        const float shakeAge = float(model.now - shake);
        if (shakeAge >= 0 && shakeAge < .3f && model.settings.fx == FxLevel::Full) {
            const float amount =
                std::clamp(model.settings.shake, 0.f, 3.f) * 4 * (1 - shakeAge / .3f);
            jitter = {std::sin(shakeAge * 150) * amount,
                      std::sin(shakeAge * 193 + 1) * amount * .6f};
        }
        for (std::size_t i = 0; i < count;) {
            auto &p = particles[i];
            p.age += dt;
            const auto *a = findAnchor(p.anchorBuffer, p.anchorId, p.viewport);
            if (p.age >= p.life || !a || p.viewport >= viewportCount) {
                remove(i);
                continue;
            }
            const Vec2 delta{a->rect.min.x - p.anchorRect.min.x,
                             a->rect.min.y - p.anchorRect.min.y};
            p.pos.x += delta.x;
            p.pos.y += delta.y;
            p.anchorRect = a->rect;
            p.clip = viewports[p.viewport];
            if (p.shape == Shape::Veil)
                p.rect = a->rect;
            const float drag = std::exp(-std::clamp(model.settings.drag, 0.f, 12.f) * p.drag * dt);
            p.vel.x *= drag;
            p.vel.y = p.vel.y * drag +
                      std::clamp(model.settings.gravity, -500.f, 1000.f) * p.gravity * dt;
            p.pos.x += p.vel.x * dt;
            p.pos.y += p.vel.y * dt;
            p.angle += p.spin * dt;
            ++i;
        }
    }
    void render(RenderBackend &backend, const CustomDrawContext &context) const {
        if (!count || context.opacity <= 0 || context.surfaceWidth <= 0 ||
            context.surfaceHeight <= 0)
            return;
        auto &resources = backend.extension<ParticleResources>();
        resources.initialize(backend);
        const NativeState saved;
        backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
        const float scale = std::max(context.pixelScaleX, context.pixelScaleY);
        for (std::size_t i = 0; i < count; ++i) {
            const auto &p = particles[i];
            int x0 = static_cast<int>(std::ceil(std::max(0.f, p.clip.min.x) * context.pixelScaleX));
            int x1 = static_cast<int>(
                std::floor(std::min(context.surfaceWidth, p.clip.max.x) * context.pixelScaleX));
            int y0 = static_cast<int>(
                std::ceil((context.surfaceHeight - std::min(context.surfaceHeight, p.clip.max.y)) *
                          context.pixelScaleY));
            int y1 = static_cast<int>(std::floor(
                (context.surfaceHeight - std::max(0.f, p.clip.min.y)) * context.pixelScaleY));
            if (saved.clipped) {
                x0 = std::max(x0, saved.scissor[0]);
                x1 = std::min(x1, saved.scissor[0] + saved.scissor[2]);
                y0 = std::max(y0, saved.scissor[1]);
                y1 = std::min(y1, saved.scissor[1] + saved.scissor[3]);
            }
            if (x1 <= x0 || y1 <= y0)
                continue;
            backend.setScissor({x0, y0, x1 - x0, y1 - y0});
            const float t = p.age / p.life;
            Vec2 pos{p.pos.x + jitter.x, p.pos.y + jitter.y};
            const float fade = std::sin(std::min(1.f, t / .08f) * pi * .5f) * (1 - fx::smooth(t));
            const auto tint = color(p.tint, fade * .7f * context.opacity);
            if (p.shape == Shape::Veil) {
                const float envelope = p.life > 1 ? fx::veil(p.age) : std::sin(t * pi);
                const float pad = 8 * fx::smooth(std::min(1.f, p.age / .3f));
                const Color c = withAlpha(p.tint, .095f * envelope * context.opacity *
                                                      (level == FxLevel::Subtle ? .55f : 1.f));
                softQuad(backend, context, resources.dot.id, center(p.rect),
                         {(p.rect.max.x - p.rect.min.x) * .5f + pad,
                          (p.rect.max.y - p.rect.min.y) * .5f + 12},
                         0, c);
            } else if (p.shape == Shape::Ring) {
                const float radius = 2 + p.size * fx::cubicOut(t);
                nativeRect(backend, context, pos, {radius, radius}, 0,
                           color(p.tint, std::sin(t * pi) * .38f * context.opacity), radius, true,
                           1.5f * (1 - t) + .5f);
            } else if (p.shape == Shape::Fragment && p.glyphRun < glyphRuns.size()) {
                const auto &run = glyphRuns[p.glyphRun];
                const auto &r = resources.rasterize(backend, run, p.glyphRun, scale);
                const auto span = r.spans[p.glyph];
                const float width = span.right - span.left;
                if (width > 0)
                    backend.drawGlyph(
                        {.texture = r.framebuffer->colorTexture(),
                         .surfaceWidth = context.surfaceWidth,
                         .surfaceHeight = context.surfaceHeight,
                         .width = width,
                         .height = r.height,
                         .u0 = (span.left - r.left) / r.width,
                         .v0 = 1,
                         .u1 = (span.right - r.left) / r.width,
                         .v1 = 0,
                         .opacity = tint.a,
                         .transform =
                             Mat3::translation(pos.x + (span.left + span.right) * .5f, pos.y) *
                             Mat3::rotation(p.angle) *
                             Mat3::translation(-width * .5f, -r.height * .5f)});
            } else if (p.shape == Shape::Spark) {
                const Vec2 trail{p.vel.x * .022f, p.vel.y * .022f};
                const float length = std::hypot(trail.x, trail.y);
                nativeRect(backend, context, {pos.x - trail.x * .5f, pos.y - trail.y * .5f},
                           {std::max(.5f, length * .5f), std::max(.15f, p.size * (1 - t) * .5f)},
                           std::atan2(trail.y, trail.x), tint, .5f);
                softQuad(backend, context, resources.dot.id, pos, {5, 5}, 0,
                         color(p.tint, fade * .25f * context.opacity));
            } else if (p.shape == Shape::Fragment)
                softQuad(backend, context, resources.dot.id, pos, {p.size, p.size * .5f}, p.angle,
                         tint);
            else
                softQuad(backend, context, resources.dot.id, pos, {p.size, p.size}, 0, tint);
        }
        saved.restore(backend);
    }
};

Effects::Effects() : impl(std::make_unique<Impl>()) {}
Effects::~Effects() = default;
void Effects::attach(Node &root) {
    auto &p = *impl;
    if (p.overlay && p.overlay->parent() == &root)
        return;
    if (p.overlay && p.overlay->parent())
        p.overlay->parent()->removeChild(p.overlay);
    auto overlay = std::make_unique<Impl::Overlay>(p);
    p.overlay = overlay.get();
    overlay->setSize(root.width(), root.height());
    root.addChild(std::move(overlay));
}
void Effects::draw(Model &model) {
    auto &p = *impl;
    const auto oldCount = p.count;
    const auto invalidate = [&] {
        const auto visual = visualRevision(), theme = themeRevision();
        if (p.overlay &&
            (oldCount || p.count || p.visualVersion != visual || p.themeVersion != theme))
            p.overlay->markPaintDirty();
        p.visualVersion = visual;
        p.themeVersion = theme;
    };
    const float dt = p.previous < 0 ? 0.f : std::clamp(float(model.now - p.previous), 0.f, .05f);
    p.previous = model.now;
    p.level = model.settings.fx;
    if (p.overlay && p.overlay->parent()) {
        p.overlay->setSize(p.overlay->parent()->width(), p.overlay->parent()->height());
    }
    if (!p.audioError.empty()) {
        model.status = "Audio: " + p.audioError;
        p.audioError.clear();
    }
    if (model.settings.fx == FxLevel::Off) {
        p.stop();
        invalidate();
        expireAnchors();
        return;
    }
    if (p.mixer)
        MIX_SetMixerGain(
            p.mixer, model.settings.sound ? std::clamp(model.settings.masterVolume, 0.f, 1.f) : 0);
    for (auto &voice : p.voices)
        if (voice.track && MIX_TrackPlaying(voice.track)) {
            const auto config = p.cue(model.settings, director[voice.directive]);
            if (!model.settings.sound || !config.enabled || config.volume <= 0)
                MIX_StopTrack(voice.track, 0);
            else
                MIX_SetTrackGain(voice.track, std::clamp(config.volume, 0.f, 1.f));
        }
    if (model.settings.reducedMotion)
        p.clearMotion();
    const auto active = model.buffers.find(model.activeBuffer);
    p.fontSize = active == model.buffers.end()
                     ? presentationMetrics().fontSize * themeMessageScale()
                     : messageTextSize(model.settings, active->second);
    const std::string_view family = model.settings.messageFont.empty()
                                        ? std::string_view("sans-serif")
                                        : std::string_view(model.settings.messageFont);
    if (p.font.family != family)
        p.font.family = family;
    p.font.weight = themeMessageFont().weight;
    for (const auto &event : model.effects) {
        if (event.history)
            continue;
        std::string_view type = event.type;
        if (type == "state") {
            if (event.text == "Disconnected" || event.text == "Failed" ||
                event.text == "Failed-fatal")
                type = "disconnect";
            else
                continue; // No boot or connect effects.
        }
        for (std::size_t i = 0; i < std::size(director); ++i)
            if (director[i].event == type) {
                if (!event.silent)
                    p.sound(model.settings, director[i], i, model.now);
                if (!model.settings.reducedMotion)
                    p.event(event, director[i], model);
                break;
            }
    }
    if (!model.settings.reducedMotion) {
        p.panelMotion(model.now, model.settings.fx == FxLevel::Full);
        p.updateParticles(model, dt);
    }
    invalidate();
    expireAnchors();
}
} // namespace blast
