#include "graphics.hpp"
#include "../fx/tween.hpp"
#include "../theme/theme.hpp"
#include "../ui/presentation.hpp"
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <render/backend/render_backend.h>
#include <render/core/renderer.h>
#include <render/core/shader_program.h>
#include <stdexcept>
#include <ui/controls/image.h>

namespace blast {
namespace {
constexpr const char *vertex = R"glsl(
precision highp float;
attribute vec2 a_position;
uniform mat3 Transform;
uniform vec2 Surface, Extent;
varying vec2 uv;
void main() {
    uv = a_position;
    vec3 p = Transform * vec3(a_position * Extent, 1.0);
    gl_Position = vec4(p.x / Surface.x * 2.0 - 1.0, 1.0 - p.y / Surface.y * 2.0, 0.0, 1.0);
}
)glsl";
constexpr const char *postShader = R"glsl(
precision highp float;
varying vec2 uv;
uniform sampler2D Image;
uniform vec2 Logical;
uniform vec3 Background;
uniform float Split, Desaturate, SplitAge, BackgroundOpacity;
void main() {
    vec2 st = vec2(uv.x, 1.0 - uv.y);
    float side = uv.x < 0.5 ? -1.0 : 1.0;
    st.x -= side * Split / Logical.x;
    if (st.x < 0.0 || st.x > 1.0 || (side < 0.0 && st.x > 0.5) || (side > 0.0 && st.x < 0.5)) {
        gl_FragColor = vec4(Background, BackgroundOpacity);
        return;
    }
    vec4 core = texture2D(Image, st);
    vec3 c = core.rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(c, vec3(l), Desaturate);
    float local = SplitAge - min(floor(uv.y * Logical.y / 24.0) * 0.03, 0.3);
    float fade = Split > 0.0 && local >= 0.0 ? 1.0 - 0.35 * sin(clamp(local / 0.85, 0.0, 1.0) * 3.14159265) : 1.0;
    c = mix(Background, c, fade);
    gl_FragColor = vec4(clamp(c, 0.0, 1.0), core.a);
}
)glsl";

struct VisualState {
    std::uint64_t revision = 1;
    float split = 0, desaturate = 0, splitAge = 0;
} visuals;

// Custom draws share a GLES context with the native renderer. Restore every state we change.
struct GlState {
    GLint program{}, framebuffer{}, viewport[4]{}, scissor[4]{}, buffer{}, activeTexture{},
        texture{};
    GLint srcRgb{}, dstRgb{}, srcAlpha{}, dstAlpha{}, eqRgb{}, eqAlpha{};
    GLint attribute{}, attributeSize{}, attributeType{}, attributeNormalized{}, attributeStride{},
        attributeBuffer{};
    void *attributePointer = nullptr;
    GLboolean blend{}, scissorEnabled{};
    GlState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqAlpha);
        blend = glIsEnabled(GL_BLEND);
        scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribute);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attributeSize);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attributeType);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attributeNormalized);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attributeStride);
        glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attributeBuffer);
        glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attributePointer);
    }
    ~GlState() {
        restoreTarget();
        glUseProgram(program);
        glBindBuffer(GL_ARRAY_BUFFER, attributeBuffer);
        glVertexAttribPointer(0, attributeSize, attributeType, attributeNormalized, attributeStride,
                              attributePointer);
        if (attribute)
            glEnableVertexAttribArray(0);
        else
            glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glActiveTexture(activeTexture);
        glBlendFuncSeparate(srcRgb, dstRgb, srcAlpha, dstAlpha);
        glBlendEquationSeparate(eqRgb, eqAlpha);
        if (blend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
    }
    void restoreTarget() const {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
        if (scissorEnabled)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);
    }
};

struct Program {
    ShaderProgram shader;
    GLint transform{}, surface{}, extent{};
    void create(const char *fragment) {
        shader.create(vertex, fragment);
        // All programs contain exactly one attribute; the linker assigns index zero.
        if (glGetAttribLocation(shader.id(), "a_position") != 0)
            throw std::runtime_error("native postprocess position attribute must be index zero");
        transform = location("Transform");
        surface = location("Surface");
        extent = location("Extent");
    }
    GLint location(const char *name) const { return glGetUniformLocation(shader.id(), name); }
    void use(const CustomDrawContext &context) const {
        glUseProgram(shader.id());
        glUniformMatrix3fv(transform, 1, GL_FALSE, context.transform.m.data());
        glUniform2f(surface, context.surfaceWidth, context.surfaceHeight);
        glUniform2f(extent, context.width, context.height);
    }
};
class VisualPostprocess final : public RenderExtension {
  public:
    Program postProgram;
    GLuint buffer = 0;
    GLuint snapshot = 0;
    int snapshotWidth = 0, snapshotHeight = 0;
    GLint postImage{}, postLogical{}, postBackground{}, postBackgroundOpacity{}, postSplit{},
        postDesaturate{}, postAge{};
    void initialize() {
        if (buffer)
            return;
        postProgram.create(postShader);
        constexpr float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        postImage = postProgram.location("Image");
        postLogical = postProgram.location("Logical");
        postBackground = postProgram.location("Background");
        postBackgroundOpacity = postProgram.location("BackgroundOpacity");
        postSplit = postProgram.location("Split");
        postDesaturate = postProgram.location("Desaturate");
        postAge = postProgram.location("SplitAge");
    }
    void destroy() override {
        if (buffer)
            glDeleteBuffers(1, &buffer);
        if (snapshot)
            glDeleteTextures(1, &snapshot);
        buffer = snapshot = 0;
        snapshotWidth = snapshotHeight = 0;
        postProgram.shader.destroy();
    }
    void abandon() noexcept override {
        buffer = snapshot = 0;
        snapshotWidth = snapshotHeight = 0;
        postProgram.shader.abandon();
    }
    void quad() const {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    static void texture(GLuint value) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, value);
    }
};

Color alpha(Color color, float amount) {
    color.a *= amount;
    return color;
}
void rect(RenderBackend &backend, const CustomDrawContext &context, Color fill, Color border,
          float rounding, float borderWidth) {
    RoundedRectStyle style;
    style.fill = alpha(fill, context.opacity);
    style.border = alpha(border, context.opacity);
    style.radius = rounding;
    style.borderWidth = borderWidth;
    backend.drawRect(context.surfaceWidth, context.surfaceHeight, context.width, context.height,
                     style, context.transform);
}
} // namespace

struct MaterialNode::Impl {
    MaterialKind kind;
    bool own = false, presence = false, hovered = false, highlight = false;
    float opacity = 1, radius = -1;
    std::uint64_t theme = 0;
    explicit Impl(MaterialKind value) : kind(value) {}
};
MaterialNode::MaterialNode(MaterialKind kind) : impl(std::make_unique<Impl>(kind)) {}
MaterialNode::~MaterialNode() = default;
void MaterialNode::setMessage(const MessageSurface &surface) {
    auto &p = *impl;
    if (p.own == surface.own && p.presence == surface.presence && p.hovered == surface.hovered &&
        p.highlight == surface.highlight && p.opacity == surface.opacity)
        return;
    p.own = surface.own;
    p.presence = surface.presence;
    p.hovered = surface.hovered;
    p.highlight = surface.highlight;
    p.opacity = surface.opacity;
    markPaintDirty();
}
void MaterialNode::sync() {
    const float radius =
        impl->kind == MaterialKind::Message ? themeBubbleRadius() : themeCornerRadius();
    if (impl->theme != themeRevision() || impl->radius != radius) {
        impl->theme = themeRevision();
        impl->radius = radius;
        markPaintDirty();
    }
}
LayoutSize MaterialNode::doMeasure(Renderer &renderer, const LayoutConstraints &constraints) {
    LayoutSize result{};
    for (const auto &child : children())
        if (child->visible() && child->participatesInLayout()) {
            const auto size = child->measure(renderer, constraints);
            result.width = std::max(result.width, size.width);
            result.height = std::max(result.height, size.height);
        }
    return constraints.constrain(result);
}
void MaterialNode::doLayout(Renderer &renderer) {
    for (const auto &child : children())
        if (child->visible()) {
            if (child->participatesInLayout())
                child->arrange(renderer, {0, 0, width(), height()});
            else
                child->layout(renderer);
        }
}
void MaterialNode::doArrange(Renderer &renderer, const LayoutRect &bounds) {
    setPosition(bounds.x, bounds.y);
    setSize(bounds.width, bounds.height);
    doLayout(renderer);
}
void MaterialNode::draw(RenderBackend &backend, const CustomDrawContext &incoming) const {
    const auto &p = *impl;
    auto context = incoming;
    context.opacity *= p.opacity;
    if (context.width <= 0 || context.height <= 0 || context.opacity <= 0)
        return;
    const float scale = themeScale();
    const float radius =
        p.kind == MaterialKind::Message ? themeBubbleRadius() : themeCornerRadius();
    // Routine events share the reading surface, not a conversational bubble/accent.
    if (p.kind == MaterialKind::Message && p.presence && !p.hovered && !p.highlight)
        return;
    const bool own = p.own && !p.presence;
    const auto panelBorder = [&p] {
        const bool conversation = p.kind == MaterialKind::Conversation;
        return withAlpha(themeForegroundColor(conversation ? "primary" : "outline_variant"),
                         conversation ? .7f : .6f);
    };
    if (p.kind == MaterialKind::Backdrop) {
        rect(backend, context, themeSurfaceColor("background"), rgba(0, 0, 0, 0), 0, 0);
    } else if (p.kind == MaterialKind::Panel || p.kind == MaterialKind::Conversation) {
        rect(backend, context, themeSurfaceColor("surface_container_low"), panelBorder(), radius,
             scale);
    } else {
        auto fill = themeColor(p.presence ? "surface_container_low"
                               : own      ? "primary_container"
                                          : "surface_container_high");
        if (p.highlight)
            fill = lerpColor(fill, themeColor("tertiary_container"), .35f);
        if (p.hovered)
            fill = lerpColor(fill, themeColor("tertiary_container"), .15f);
        rect(backend, context, themeSurfaceFill(fill),
             withAlpha(themeColor(p.highlight || p.hovered ? "tertiary" : "outline_variant"),
                       p.highlight ? .55f
                       : p.hovered ? .35f
                                   : .3f),
             radius, scale);
    }
}

PixelRevealNode::PixelRevealNode(Node *value) : source(value) {
    setParticipatesInLayout(false);
    setHitTestVisible(false);
    setExcludeSubtreeFromTabOrder(true);
}
PixelRevealNode::~PixelRevealNode() = default;
void PixelRevealNode::clearTiles() {
    while (!children().empty())
        removeChild(children().back().get());
}
void PixelRevealNode::setSource(Node *value) {
    if (source == value)
        return;
    clearTiles();
    source = value;
}
void PixelRevealNode::setRevealBounds(const LayoutRect &value) { bounds = value; }
void PixelRevealNode::setReveal(float progress, float opacity, bool pixels) {
    if (!source)
        return;
    progress = std::isfinite(progress) ? std::clamp(progress, 0.f, 1.f) : 1.f;
    this->progress = progress;
    if (!pixels || progress >= 1 || source->width() <= 0 || source->height() <= 0) {
        source->setOpacity(opacity * (pixels ? 1.f : progress));
        clearTiles();
        setVisible(false);
        return;
    }
    // ponytail: at most 64 disjoint clipped subtree traversals per entering row;
    // completed rows use their ordinary sharp tree without proxies.
    if (children().empty())
        for (unsigned i = 0; i < 64; ++i) {
            auto tile = std::make_unique<Node>();
            tile->setParticipatesInLayout(false);
            tile->setHitTestVisible(false);
            tile->setClipChildren(true);
            tile->addChild(std::make_unique<RenderProxyNode>(source));
            addChild(std::move(tile));
        }
    source->setOpacity(0);
    setVisible(true);
    setOpacity(opacity);
    setPosition(source->x(), source->y());
    setSize(source->width(), source->height());
    layoutTiles();
}
void PixelRevealNode::layoutTiles() {
    const LayoutRect area =
        bounds.width > 0 && bounds.height > 0 ? bounds : LayoutRect{0, 0, width(), height()};
    const unsigned columns = static_cast<unsigned>(
        std::clamp(std::round(std::sqrt(64.f * area.width / area.height)), 1.f, 64.f));
    const unsigned rows = std::max(1U, 64U / columns);
    std::array<float, 65> xEdges{}, yEdges{};
    for (unsigned i = 0; i <= columns; ++i)
        xEdges[i] = area.x + float(i) * area.width / columns;
    for (unsigned i = 0; i <= rows; ++i)
        yEdges[i] = area.y + float(i) * area.height / rows;
    for (unsigned i = 0; i < children().size(); ++i) {
        auto &tile = *children()[i];
        tile.setVisible(i < columns * rows);
        if (i >= columns * rows)
            continue;
        // Every mask has the SAME world matrix and reuses identical shared edges;
        // native draw-time integer scissors cannot overlap at fractional translations.
        tile.setPosition(0, 0);
        tile.setSize(width(), height());
        tile.setPixelPartitionClip(xEdges[i % columns], yEdges[i / columns],
                                   xEdges[i % columns + 1], yEdges[i / columns + 1]);
        tile.setOpacity(fx::messageBlockOpacity(progress, i));
        tile.children().front()->setPosition(0, 0);
    }
}

void setVisualImpulse(float distance, float desaturate, float age) {
    auto &v = visuals;
    if (distance <= 0)
        age = 0;
    if (v.split != distance || v.desaturate != desaturate || v.splitAge != age)
        ++v.revision;
    v.split = distance;
    v.desaturate = desaturate;
    v.splitAge = age;
}
std::uint64_t visualRevision() { return visuals.revision; }
void drawVisualPostprocess(RenderBackend &backend, const CustomDrawContext &context) {
    auto &v = visuals;
    if (v.split == 0 && v.desaturate == 0)
        return;
    GlState saved;
    auto &gpu = backend.extension<VisualPostprocess>();
    gpu.initialize();
    const int width = saved.viewport[2], height = saved.viewport[3];
    if (width <= 0 || height <= 0)
        return;
    if (!gpu.snapshot)
        glGenTextures(1, &gpu.snapshot);
    VisualPostprocess::texture(gpu.snapshot);
    if (gpu.snapshotWidth != width || gpu.snapshotHeight != height) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        gpu.snapshotWidth = width;
        gpu.snapshotHeight = height;
    }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, saved.viewport[0], saved.viewport[1], width,
                        height);
    auto full = context;
    full.transform = Mat3::identity();
    full.width = full.surfaceWidth;
    full.height = full.surfaceHeight;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    gpu.postProgram.use(full);
    glUniform1i(gpu.postImage, 0);
    glUniform2f(gpu.postLogical, std::max(context.surfaceWidth, 1.f),
                std::max(context.surfaceHeight, 1.f));
    const auto background = themeColor("background");
    glUniform3f(gpu.postBackground, background.r, background.g, background.b);
    glUniform1f(gpu.postBackgroundOpacity, themeSurfaceAlpha());
    glUniform1f(gpu.postSplit, v.split);
    glUniform1f(gpu.postDesaturate, v.desaturate);
    glUniform1f(gpu.postAge, v.splitAge);
    gpu.quad();
}

struct LucideIcon::Impl {
    std::string name, path;
    Image *image = nullptr;
    int rasterSize = 0;
    explicit Impl(std::string_view value) : name(value) {}
};
LucideIcon::LucideIcon(std::string_view name, float size) : impl(std::make_unique<Impl>(name)) {
    auto image = std::make_unique<Image>();
    impl->image = image.get();
    image->setFit(ImageFit::Contain);
    image->setForegroundTint(colorSpecFromRole(ColorRole::OnSurface));
    image->setHitTestVisible(false);
    addChild(std::move(image));
    setSize(size, size);
    setHitTestVisible(false);
}
LucideIcon::~LucideIcon() = default;
void LucideIcon::setName(std::string_view name) {
    if (impl->name == name)
        return;
    impl->name = name;
    impl->path.clear();
    impl->rasterSize = 0;
    markLayoutDirty();
}
void LucideIcon::setColor(Color color) { impl->image->setForegroundTint(fixedColorSpec(color)); }
void LucideIcon::doLayout(Renderer &renderer) {
    auto &p = *impl;
    // Names come from application actions, never from IRC text or filesystem paths.
    if (p.name.empty() ||
        p.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
        throw std::runtime_error("invalid Lucide icon name");
    if (p.path.empty()) {
        std::array<std::filesystem::path, 3> roots;
#ifdef BLASTIRC_ASSET_DIR
        roots[0] = BLASTIRC_ASSET_DIR;
#endif
        roots[1] = "assets";
        std::error_code error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error)
            roots[2] = executable.parent_path() / "../share/blastirc/assets";
        for (const auto &root : roots) {
            if (root.empty())
                continue;
            auto path = root / "lucide" / (p.name + ".svg");
            if (std::filesystem::is_regular_file(path, error)) {
                p.path = path.string();
                break;
            }
        }
        if (p.path.empty())
            throw std::runtime_error("missing Lucide SVG: " + p.name);
    }
    const int rasterSize = std::max(
        1, static_cast<int>(std::ceil(std::max(width(), height()) * renderer.renderScale())));
    if (!p.image->hasImage() || p.rasterSize != rasterSize) {
        const int logicalSize =
            std::max(1, static_cast<int>(std::ceil(std::max(width(), height()))));
        if (!p.image->setSourceFile(renderer, p.path, logicalSize))
            throw std::runtime_error("cannot decode Lucide SVG: " + p.path);
        p.rasterSize = rasterSize;
    }
    p.image->setSize(width(), height());
    Node::doLayout(renderer);
}
LayoutSize LucideIcon::doMeasure(Renderer &, const LayoutConstraints &constraints) {
    return constraints.constrain({width(), height()});
}
void LucideIcon::doArrange(Renderer &renderer, const LayoutRect &bounds) {
    setPosition(bounds.x, bounds.y);
    setSize(bounds.width, bounds.height);
    impl->image->arrange(renderer, {0, 0, bounds.width, bounds.height});
    doLayout(renderer);
}
} // namespace blast
