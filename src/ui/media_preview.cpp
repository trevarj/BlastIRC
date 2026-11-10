#include "media_preview.hpp"
#include "../model.hpp"
#include "../preview.hpp"
#include "../render/graphics.hpp"
#include "../theme/theme.hpp"
#include "media_player.hpp"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <core/input/key_modifiers.h>
#include <core/text_clipboard.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <render/codecs/codecs.h>
#include <render/core/image_decoder.h>
#include <render/core/image_resize.h>
#include <render/core/renderer.h>
#include <render/core/texture_manager.h>
#include <render/scene/input_dispatcher.h>
#include <set>
#include <tuple>
#include <ui/controls/box.h>
#include <ui/controls/button.h>
#include <ui/controls/image.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/select_popup_context.h>
#include <ui/controls/slider.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace blast {
namespace {
class PreviewInfoText final : public Input {
  public:
    std::string content;
    PreviewInfoText() {
        setMultiline(true);
        setOnChange([this](const std::string &) { setValue(content); });
        setOnKeyEvent([this](std::uint32_t sym, std::uint32_t modifiers) {
            if (modifiers == KeyMod::Ctrl && sym == XKB_KEY_a) {
                selectAll();
                return true;
            }
            const bool copy = modifiers == KeyMod::Ctrl && sym == XKB_KEY_c;
            const bool move = sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_Up ||
                              sym == XKB_KEY_Down || sym == XKB_KEY_Home || sym == XKB_KEY_End ||
                              sym == XKB_KEY_Page_Up || sym == XKB_KEY_Page_Down;
            return !copy && !move;
        });
    }
    void setContent(std::string text) {
        content = std::move(text);
        setValue(content);
    }
    void textInputApplyEdit(const TextInputEdit &) override {}
};
template <class T> T *child(Node &parent) {
    auto node = std::make_unique<T>();
    auto *ptr = node.get();
    parent.addChild(std::move(node));
    return ptr;
}
PreviewKind urlKind(std::string_view url) {
    auto end = url.find_first_of("?#");
    std::string path(url.substr(0, end));
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto dot = path.rfind('.');
    auto ext = dot == std::string::npos ? std::string_view{} : std::string_view(path).substr(dot);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp")
        return PreviewKind::Image;
    if (ext == ".mp3" || ext == ".ogg" || ext == ".opus" || ext == ".wav" || ext == ".flac" ||
        ext == ".m4a")
        return PreviewKind::Audio;
    if (ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".mov")
        return PreviewKind::Video;
    return PreviewKind::Link;
}
std::string hostOf(std::string_view url) {
    auto start = url.find("://");
    if (start == url.npos)
        return {};
    auto host = url.substr(start + 3);
    return std::string(host.substr(0, host.find_first_of("/?#")));
}
void labelStyle(Label &label, float scale) {
    const auto &font = themeUiFont();
    label.setUseMarkup(false);
    label.setMaxLines(1);
    label.setFontFamily(font.family);
    label.setFontFeatures(font.features);
    label.setFontWeight(font.weight);
    label.setFontSize(13 * scale);
    label.setColor(themeForegroundColor("on_surface"));
}
bool loadRaster(Image &image, Renderer &renderer, const PreviewAsset &asset, int target = 0) {
    if (!asset.bytes || asset.bytes > 32ULL * 1024 * 1024)
        return false;
    std::ifstream input(asset.path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != static_cast<std::streamoff>(asset.bytes))
        return false;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(asset.bytes));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        return false;
    auto raster = PreviewService::raster(bytes);
    if (!raster || !raster->width || !raster->height ||
        static_cast<std::uint64_t>(raster->width) * raster->height > 16ULL * 1024 * 1024)
        return false;
    auto decoded = [&]() -> std::expected<DecodedRasterImage, std::string> {
        if (raster->extension != ".gif")
            return decodeRasterImage(bytes.data(), bytes.size());
        // Static first frame only: never hand remote GIFs to the unbounded animation path.
        auto gif = codecs::decodeAnimatedGif(bytes.data(), bytes.size(), 1, 64ULL * 1024 * 1024);
        if (!gif || gif->width != static_cast<int>(raster->width) ||
            gif->height != static_cast<int>(raster->height) || gif->frames.size() != 1)
            return std::unexpected("GIF first frame could not be decoded safely");
        return DecodedRasterImage{std::move(gif->frames.front().rgba), gif->width, gif->height};
    }();
    if (!decoded ||
        !((decoded->width == static_cast<int>(raster->width) &&
           decoded->height == static_cast<int>(raster->height)) ||
          (raster->extension == ".jpg" && decoded->width == static_cast<int>(raster->height) &&
           decoded->height == static_cast<int>(raster->width))) ||
        decoded->pixels.size() != static_cast<std::uint64_t>(decoded->width) * decoded->height * 4)
        return false;
    if (target > 0 && std::max(decoded->width, decoded->height) > target) {
        const double factor =
            static_cast<double>(target) / std::max(decoded->width, decoded->height);
        const int w = std::max(1, static_cast<int>(decoded->width * factor));
        const int h = std::max(1, static_cast<int>(decoded->height * factor));
        auto resized = resizeToRgba(decoded->pixels.data(), decoded->width, decoded->height,
                                    ResizeSourceLayout::Rgba, w, h);
        return !resized.empty() && image.setSourceRaw(renderer, resized.data(), resized.size(), w,
                                                      h, 0, PixmapFormat::RGBA);
    }
    return image.setSourceRaw(renderer, decoded->pixels.data(), decoded->pixels.size(),
                              decoded->width, decoded->height, 0, PixmapFormat::RGBA);
}
std::string clockText(double seconds) {
    const auto n = static_cast<unsigned>(std::max(0., std::isfinite(seconds) ? seconds : 0.));
    char text[32];
    std::snprintf(text, sizeof text, "%u:%02u", n / 60, n % 60);
    return text;
}
} // namespace

struct MediaPreviewController::Impl {
    struct Entry {
        std::string origin, url, host, title, imageUrl, error;
        PreviewKind kind = PreviewKind::Link;
        std::string original, description;
        PreviewRaster raster;
        double duration = 0;
        std::shared_ptr<const PreviewAsset> asset;
        std::uint64_t ticket = 0, assetRevision = 0;
        bool pending = false, fetched = false, automaticAllowed = true, needsConsent = false;
        PreviewKind requestedKind = PreviewKind::Link;
        std::uint64_t lastUse = 0;
        unsigned attemptedKinds = 0, metadataImages = 0;
    };
    struct Overlay final : Node {
        Impl &state;
        InputArea *blocker, *panel;
        Box *background;
        Image *image;
        Label *title, *status;
        Button *dismiss, *play, *consent;
        PreviewInfoText *info;
        Slider *seek, *volume;
        Renderer *renderer = nullptr;
        TextureHandle texture{};
        std::uint64_t uploadedRevision = 0;
        const PreviewAsset *shown = nullptr;
        float scale = 1;
        bool settingControls = false;
        explicit Overlay(Impl &s) : state(s) {
            setParticipatesInLayout(false);
            setZIndex(1100);
            setVisible(false);
            setHitTestVisible(false);
            blocker = child<InputArea>(*this);
            blocker->setFocusable(false);
            blocker->setTabStop(false);
            blocker->setOnClick([this](const auto &) { state.close(true); });
            blocker->setOnAxisHandler([](const auto &) { return true; });
            panel = child<InputArea>(*this);
            panel->setFocusable(false);
            panel->setTabStop(false);
            panel->setClipChildren(true);
            panel->setOnAxisHandler([](const auto &) { return true; });
            background = child<Box>(*panel);
            background->setHitTestVisible(false);
            image = child<Image>(*panel);
            image->setFit(ImageFit::Contain);
            image->setHitTestVisible(false);
            title = child<Label>(*panel);
            status = child<Label>(*panel);
            info = child<PreviewInfoText>(*panel);
            info->inputArea()->setTabFocusKey("blastirc.media.info");
            info->setVisible(false);
            for (auto *label : {title, status})
                label->setHitTestVisible(false);
            dismiss = child<Button>(*panel);
            dismiss->setText("Close");
            dismiss->setOnClick([this] { state.close(true); });
            play = child<Button>(*panel);
            play->setText("Play");
            play->setOnClick([this] { state.player.playPause(); });
            dismiss->inputArea()->setTabFocusKey("blastirc.media.close");
            play->inputArea()->setTabFocusKey("blastirc.media.play");
            consent = child<Button>(*panel);
            consent->setVisible(false);
            consent->inputArea()->setTabFocusKey("blastirc.media.load-type");
            consent->setOnClick([this] { state.activate(state.overlayId); });
            seek = child<Slider>(*panel);
            seek->setRange(0, 1);
            seek->setStep(.1);
            seek->setOnValueChanged([this](double value) {
                if (!settingControls)
                    state.player.seek(value);
            });
            volume = child<Slider>(*panel);
            volume->setRange(0, 100);
            volume->setValue(100);
            volume->setOnValueChanged([this](double value) {
                if (!settingControls)
                    state.player.volume(value);
            });
        }
        ~Overlay() override { release(); }
        void release() {
            if (renderer) {
                image->clear(*renderer);
                if (texture.valid())
                    renderer->textureManager().unload(texture);
            }
            texture = {};
            shown = nullptr;
            uploadedRevision = 0;
        }
        void doInvalidateGpuResources(Renderer &) override {
            texture = {};
            shown = nullptr;
            uploadedRevision = 0;
            renderer = nullptr;
        }
        void doRebindRenderer(Renderer &r) override { renderer = &r; }
        void doLayout(Renderer &r) override {
            renderer = &r;
            blocker->setSize(width(), height());
            const float margin = std::min({16 * scale, width() / 8, height() / 8});
            const float w = std::max(0.f, std::min(1000 * scale, width() - 2 * margin));
            const float h = std::max(0.f, std::min(740 * scale, height() - 2 * margin));
            panel->setPosition((width() - w) / 2, (height() - h) / 2);
            panel->setSize(w, h);
            background->setSize(w, h);
            background->setFill(themeSurfaceColor("surface_container_high"));
            background->setRadius(themeCornerRadius());
            const float pad = std::min(12 * scale, w / 8), inner = std::max(0.f, w - 2 * pad);
            for (auto *label : {title, status}) {
                labelStyle(*label, scale);
                label->setMaxWidth(inner);
            }
            title->setPosition(pad, pad);
            title->setMaxWidth(std::max(0.f, inner - 90 * scale));
            dismiss->setPosition(std::max(pad, w - pad - 80 * scale), pad);
            dismiss->setSize(80 * scale, 32 * scale);
            const bool media =
                state.overlayKind == PreviewKind::Audio || state.overlayKind == PreviewKind::Video;
            const auto entry = state.entries.find(state.overlayId);
            const bool waiting = entry != state.entries.end() && entry->second.needsConsent;
            const float bottom = media || waiting ? 112 * scale : 40 * scale;
            image->setPosition(pad, 52 * scale);
            image->setSize(inner, std::max(0.f, h - 52 * scale - bottom));
            status->setPosition(pad, std::max(0.f, h - bottom));
            info->setPosition(pad, 52 * scale);
            info->setSize(inner, std::max(0.f, h - 104 * scale));
            info->setFontSize(14 * scale);
            if (state.showingInfo) {
                image->setVisible(false);
                status->setVisible(false);
                for (auto *control : {play, consent})
                    control->setVisible(false);
                seek->setVisible(false);
                volume->setVisible(false);
                Node::doLayout(r);
                return;
            }
            image->setVisible(true);
            status->setVisible(true);
            play->setVisible(media && !waiting);
            seek->setVisible(media && !waiting);
            volume->setVisible(media && !waiting);
            consent->setVisible(waiting && state.mode(entry->second.kind) != PreviewMode::Off);
            consent->setPosition(pad, std::max(0.f, h - 48 * scale));
            consent->setSize(std::min(inner, 240 * scale), 32 * scale);
            if (waiting)
                consent->setText(entry->second.kind == PreviewKind::Image   ? "Load image"
                                 : entry->second.kind == PreviewKind::Audio ? "Load audio"
                                 : entry->second.kind == PreviewKind::Video ? "Load video"
                                                                            : "Load link preview");
            play->setPosition(pad, std::max(0.f, h - 48 * scale));
            play->setSize(80 * scale, 32 * scale);
            seek->setPosition(pad + 92 * scale, std::max(0.f, h - 48 * scale));
            seek->setSize(std::max(0.f, inner - 250 * scale), 32 * scale);
            volume->setPosition(std::max(pad, w - pad - 140 * scale),
                                std::max(0.f, h - 48 * scale));
            volume->setSize(140 * scale, 32 * scale);
            for (auto *button : {dismiss, play, consent})
                button->setControlHeight(32 * scale);
            seek->setControlHeight(32 * scale);
            volume->setControlHeight(32 * scale);
            if (state.overlayKind == PreviewKind::Image && state.overlayAsset &&
                shown != state.overlayAsset.get()) {
                release();
                renderer = &r;
                status->setText(loadRaster(*image, r, *state.overlayAsset)
                                    ? "Click outside or Escape to close"
                                    : "This image could not be decoded safely.");
                shown = state.overlayAsset.get();
            }
            const auto frame = state.player.frame();
            if (media && !frame.empty() &&
                (!texture.valid() || uploadedRevision != state.player.frameRevision())) {
                // TextureManager mutates these GLES bindings; preserve the host renderer's state.
                GLint binding = 0, unpack = 4;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
                glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
                auto &textures = r.textureManager();
                if (!texture.valid())
                    texture = textures.createEmpty(state.player.width(), state.player.height(),
                                                   TextureDataFormat::Rgba);
                if (texture.valid() &&
                    textures.updateSubImage(texture, frame.data(), 0, 0, state.player.width(),
                                            state.player.height(), TextureDataFormat::Rgba)) {
                    image->setExternalTexture(r, texture);
                    image->markPaintDirty();
                    uploadedRevision = state.player.frameRevision();
                } else
                    status->setText("Video texture upload failed.");
                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(binding));
                glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
            }
            Node::doLayout(r);
        }
    };
    Model &model;
    Node &host;
    Overlay *overlay;
    InputDispatcher *dispatcher = nullptr;
    InputDispatcher::TabFocusSnapshot returnFocus;
    PreviewService service;
    MediaPlayer player;
    Label captionMeasure;
    std::map<std::uint64_t, Entry> entries;
    std::map<std::uint64_t, std::uint64_t> tickets;
    std::function<void()> changed;
    std::uint64_t generation = 1, nextTicket = 1, overlayId = 0, clock = 0;
    PreviewKind overlayKind = PreviewKind::Image;
    std::shared_ptr<const PreviewAsset> overlayAsset;
    std::uint64_t directSourceId = 0;
    bool showingInfo = false;
    using Policy = std::tuple<PreviewMode, PreviewMode, PreviewMode, int, int, int, bool>;
    Policy policy;
    explicit Impl(Model &m, Node &root, std::string caBundle)
        : model(m), host(root), service(std::move(caBundle)), policy(currentPolicy()) {
        auto view = std::make_unique<Overlay>(*this);
        overlay = view.get();
        host.addChild(std::move(view));
    }
    static void prepareCaption(Label &label, const Entry &entry, float width, float scale) {
        labelStyle(label, scale);
        label.setMaxWidth(std::max(1.f, std::min(width, 320 * scale) - 16 * scale));
        const bool page = entry.kind == PreviewKind::Link || !entry.imageUrl.empty();
        label.setText(page || entry.title.empty() ? entry.original : entry.title);
    }
    float captionHeight(Renderer &renderer, const Entry &entry, float width, float scale) {
        prepareCaption(captionMeasure, entry, width, scale);
        captionMeasure.measure(renderer);
        return std::max(24 * scale, captionMeasure.height() + 8 * scale);
    }
    Policy currentPolicy() const {
        const auto &s = model.settings;
        return {s.linkPreviews,     s.imagePreviews,      s.mediaPreviews,
                s.previewMaxHeight, s.previewMediaMaxMiB, s.previewMediaMaxSeconds,
                s.previewAllowLocal};
    }
    PreviewMode mode(PreviewKind kind) const {
        if (kind == PreviewKind::Image)
            return model.settings.imagePreviews;
        if (kind == PreviewKind::Audio || kind == PreviewKind::Video)
            return model.settings.mediaPreviews;
        return model.settings.linkPreviews;
    }
    void notify() {
        if (changed)
            changed();
    }
    void cancel(Entry &entry) {
        if (entry.ticket) {
            service.cancel(entry.ticket);
            tickets.erase(entry.ticket);
        }
        entry.ticket = 0;
        entry.pending = false;
    }
    struct CardPin {
        Node *node = nullptr;
        std::uint64_t id = 0;
        std::shared_ptr<const PreviewAsset> asset;
    };
    std::vector<std::weak_ptr<CardPin>> cardPins;
    void releaseCardPins(std::uint64_t id) {
        for (const auto &weak : cardPins)
            if (auto pin = weak.lock(); pin && pin->id == id)
                pin->asset.reset();
    }
    bool pinned(std::uint64_t id) const {
        if (overlay->visible() && (overlayId == id || (overlayId == 0 && directSourceId == id)))
            return true;
        for (const auto &weak : cardPins)
            if (auto pin = weak.lock(); pin && pin->id == id) {
                bool visible = true;
                for (auto *node = pin->node; node; node = node->parent())
                    visible = visible && node->visible();
                if (visible)
                    return true;
            }
        return false;
    }
    bool prepareCapacity(Entry &requesting) {
        if (requesting.kind == PreviewKind::Link)
            return true;
        for (auto it = cardPins.begin(); it != cardPins.end();) {
            auto pin = it->lock();
            if (!pin) {
                it = cardPins.erase(it);
                continue;
            }
            bool visible = true;
            for (auto *node = pin->node; node; node = node->parent())
                visible = visible && node->visible();
            if (!visible)
                pin->asset.reset();
            ++it;
        }
        const auto wanted = requesting.kind == PreviewKind::Image
                                ? 32ULL * 1024 * 1024
                                : static_cast<std::uint64_t>(
                                      std::clamp(model.settings.previewMediaMaxMiB, 1, 512)) *
                                      1024 * 1024;
        for (;;) {
            const auto usage = service.usage();
            const bool filesFull =
                usage.assets + usage.pendingAssets + 1 > PreviewService::maxAssets;
            const bool bytesTight = wanted > PreviewService::maxAssetBytes - usage.bytes;
            if (!filesFull && !bytesTight)
                return true;
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                auto &entry = it->second;
                if (&entry == &requesting || pinned(it->first) || !entry.asset ||
                    entry.asset.use_count() != 1)
                    continue;
                if (oldest == entries.end() || entry.lastUse < oldest->second.lastUse)
                    oldest = it;
            }
            if (oldest != entries.end()) {
                oldest->second.asset.reset();
                oldest->second.assetRevision = 0;
                oldest->second.fetched = false;
                continue;
            }
            // A size limit is an upper bound, not a reservation. Pinned assets remain
            // owned; the transport still enforces actual bytes at its atomic save.
            if (!filesFull && usage.bytes < PreviewService::maxAssetBytes)
                return true;
            requesting.error = "Preview cache is in use. Close another preview or try again after "
                               "downloads finish.";
            return false;
        }
    }
    void syncConsent(const Entry &entry) {
        overlay->consent->setVisible(entry.needsConsent && mode(entry.kind) != PreviewMode::Off);
        overlay->consent->setText(entry.kind == PreviewKind::Image   ? "Load image"
                                  : entry.kind == PreviewKind::Audio ? "Load audio"
                                  : entry.kind == PreviewKind::Video ? "Load video"
                                                                     : "Load link preview");
    }
    void request(std::uint64_t id, Entry &entry, bool explicitClick) {
        if (entry.pending || mode(entry.kind) == PreviewMode::Off ||
            (!explicitClick &&
             (!entry.automaticAllowed || mode(entry.kind) != PreviewMode::Automatic)))
            return;
        const unsigned bit = 1U << static_cast<unsigned>(entry.kind);
        if (explicitClick) {
            entry.attemptedKinds = 0;
            entry.metadataImages = 0;
        }
        if ((entry.attemptedKinds & bit) != 0) {
            entry.error = "The server changed media types repeatedly. Click to retry.";
            return;
        }
        entry.attemptedKinds |= bit;
        if (!prepareCapacity(entry))
            return;
        entry.needsConsent = false;
        entry.requestedKind = entry.kind;
        entry.lastUse = ++clock;
        entry.error.clear();
        entry.ticket = nextTicket++;
        PreviewService::Request request{
            entry.ticket,
            generation,
            entry.url,
            entry.kind,
            static_cast<std::uint64_t>(std::clamp(model.settings.previewMediaMaxMiB, 1, 512)) *
                1024 * 1024,
            model.settings.previewAllowLocal};
        if (service.request(std::move(request)) != PreviewService::Admission::Queued) {
            entry.ticket = 0;
            entry.error = "Preview request unavailable.";
            return;
        }
        entry.pending = true;
        tickets[entry.ticket] = id;
    }
    void close(bool restore) {
        const bool wasActive = overlay->visible();
        if (restore && wasActive) {
            if (auto it = entries.find(overlayId); it != entries.end())
                cancel(it->second);
        }
        player.close();
        overlayAsset.reset();
        overlayId = 0;
        overlay->release();
        overlay->setVisible(false);
        overlay->setHitTestVisible(false);
        if (dispatcher && wasActive) {
            dispatcher->cancelPointerCapture();
            dispatcher->setFocus(nullptr);
            if (restore)
                dispatcher->restoreTabFocus(std::move(returnFocus));
        }
    }
    void show(std::uint64_t id, Entry &entry) {
        showingInfo = false;
        overlay->info->setVisible(false);
        if (mode(entry.kind) == PreviewMode::Off)
            return;
        auto focus =
            dispatcher && !overlay->visible() ? dispatcher->captureTabFocus() : returnFocus;
        close(false);
        returnFocus = std::move(focus);
        overlayId = id;
        overlayKind = entry.kind;
        overlayAsset = entry.asset;
        overlay->title->setText(entry.title.empty() ? entry.host : entry.title);
        overlay->status->setText(entry.error.empty()
                                     ? (entry.kind == PreviewKind::Image
                                            ? "Loading image…"
                                            : "Loading media… playback requires Play.")
                                     : entry.error);
        syncConsent(entry);
        if (entry.asset && (entry.kind == PreviewKind::Audio || entry.kind == PreviewKind::Video))
            player.load(entry.asset, std::clamp(model.settings.previewMediaMaxSeconds, 1, 7200));
        overlay->setVisible(true);
        overlay->setHitTestVisible(true);
        overlay->markLayoutDirty();
        if (dispatcher)
            dispatcher->setFocus(overlay->dismiss->inputArea());
    }
    void info(std::uint64_t id) {
        auto it = entries.find(id);
        if (it == entries.end())
            return;
        auto &entry = it->second;
        auto focus =
            dispatcher && !overlay->visible() ? dispatcher->captureTabFocus() : returnFocus;
        close(false);
        returnFocus = std::move(focus);
        overlayId = id;
        showingInfo = true;
        overlay->title->setText("Info");
        std::string text = entry.original;
        if (!entry.title.empty())
            text += "\n\n" + entry.title;
        if (!entry.description.empty())
            text += "\n" + entry.description;
        if (entry.asset) {
            text += "\n\nType: " + std::string(entry.kind == PreviewKind::Image   ? "Image"
                                               : entry.kind == PreviewKind::Audio ? "Audio"
                                                                                  : "Video");
            text += "\nSize: " + std::to_string(entry.asset->bytes) + " bytes";
            if (entry.url != entry.origin)
                text += "\nThumbnail URL: " + entry.url;
            if (entry.duration > 0)
                text += "\nDuration: " + std::to_string(entry.duration) + " seconds";
            if (entry.raster.width && entry.raster.height)
                text += "\nDimensions: " + std::to_string(entry.raster.width) + " × " +
                        std::to_string(entry.raster.height);
            if (entry.raster.extension == ".gif")
                text += "\nPreview: static first frame";
        }
        if (entry.kind == PreviewKind::Link && entry.fetched && entry.error.empty())
            text += "\n\nType: Web page";
        if (!entry.error.empty())
            text += "\n\n" + entry.error;
        overlay->info->setContent(text);
        overlay->info->setVisible(true);
        overlay->setVisible(true);
        overlay->setHitTestVisible(true);
        overlay->markLayoutDirty();
        if (dispatcher)
            dispatcher->setFocus(overlay->info->inputArea());
    }
    void activate(std::uint64_t id) {
        auto it = entries.find(id);
        if (it == entries.end())
            return;
        auto &entry = it->second;
        entry.lastUse = ++clock;
        if (entry.asset) {
            show(id, entry);
            return;
        }
        if (!entry.needsConsent && entry.fetched && entry.error.empty() &&
            entry.kind == PreviewKind::Link && entry.imageUrl.empty()) {
            info(id);
            return;
        }
        if (!entry.needsConsent && entry.fetched && entry.kind == PreviewKind::Link &&
            !entry.imageUrl.empty()) {
            if (mode(PreviewKind::Image) == PreviewMode::Off)
                return;
            cancel(entry);
            entry.url = entry.imageUrl;
            entry.kind = PreviewKind::Image;
            entry.fetched = false;
        }
        request(id, entry, true);
        show(id, entry);
        notify();
    }
};

MediaPreviewController::MediaPreviewController(Model &model, Node &host, std::string caBundle)
    : impl_(std::make_unique<Impl>(model, host, std::move(caBundle))) {}
MediaPreviewController::~MediaPreviewController() {
    invalidate();
    impl_->host.removeChild(impl_->overlay);
}
void MediaPreviewController::setInputDispatcher(InputDispatcher &dispatcher) {
    impl_->dispatcher = &dispatcher;
}
void MediaPreviewController::setChanged(std::function<void()> callback) {
    impl_->changed = std::move(callback);
}
void MediaPreviewController::invalidate() {
    auto &s = *impl_;
    ++s.generation;
    s.service.cancelAll();
    s.tickets.clear();
    s.entries.clear();
    s.close(false);
    s.policy = s.currentPolicy();
    for (const auto &weak : s.cardPins)
        if (auto pin = weak.lock())
            pin->asset.reset();
}
void MediaPreviewController::retainMessages(std::span<const std::uint64_t> ids) {
    auto &s = *impl_;
    std::set<std::uint64_t> keep(ids.begin(), ids.end());
    if (s.directSourceId && !keep.contains(s.directSourceId))
        bind(0, {}, false);
    for (auto it = s.entries.begin(); it != s.entries.end();) {
        if (it->first && !keep.contains(it->first)) {
            if (s.overlayId == it->first)
                s.close(false);
            s.releaseCardPins(it->first);
            s.cancel(it->second);
            it = s.entries.erase(it);
        } else
            ++it;
    }
}
void MediaPreviewController::bind(std::uint64_t id, std::string_view url, bool readable,
                                  bool automaticAllowed) {
    auto &s = *impl_;
    auto retire = [&] {
        s.releaseCardPins(id);
        if (s.overlay->visible() && s.overlayId == id)
            s.close(false);
        if (auto it = s.entries.find(id); it != s.entries.end()) {
            s.cancel(it->second);
            s.entries.erase(it);
        }
        if (id && s.directSourceId == id) {
            if (s.overlay->visible() && s.overlayId == 0)
                s.close(false);
            if (auto it = s.entries.find(0); it != s.entries.end()) {
                s.cancel(it->second);
                s.entries.erase(it);
            }
            s.directSourceId = 0;
        }
    };
    if (!readable || url.empty()) {
        retire();
        return;
    }
    auto normalized = PreviewService::url(url, s.model.settings.previewAllowLocal);
    if (!normalized) {
        retire();
        return;
    }
    auto it = s.entries.find(id);
    if (it != s.entries.end() && it->second.origin == *normalized &&
        it->second.automaticAllowed == automaticAllowed)
        return;
    retire();
    if (s.entries.size() >= 256) {
        auto old = s.entries.end();
        for (auto candidate = s.entries.begin(); candidate != s.entries.end(); ++candidate)
            if (!s.pinned(candidate->first) &&
                (old == s.entries.end() || candidate->second.lastUse < old->second.lastUse))
                old = candidate;
        if (old == s.entries.end())
            return;
        s.cancel(old->second);
        s.entries.erase(old);
    }
    auto &entry = s.entries[id];
    entry.original = std::string(url);
    entry.origin = *normalized;
    entry.url = *normalized;
    entry.host = hostOf(entry.url);
    entry.kind = urlKind(entry.url);
    entry.automaticAllowed = automaticAllowed;
    s.request(id, entry, false);
}
float MediaPreviewController::measure(Renderer &renderer, std::uint64_t id, float width,
                                      float scale) const {
    auto &s = *impl_;
    auto it = s.entries.find(id);
    if (it == s.entries.end() || s.mode(it->second.kind) == PreviewMode::Off || width <= 0)
        return 0;
    const auto &raster = it->second.raster;
    const float thumbWidth = std::min(width, 320 * scale);
    const float limit = std::clamp(s.model.settings.previewMaxHeight, 64, 240) * scale;
    const float desired =
        raster.width && raster.height ? thumbWidth * raster.height / raster.width : 96 * scale;
    const float caption = replacesUrl(id) ? s.captionHeight(renderer, it->second, width, scale) : 0;
    return std::clamp(desired, std::min(64 * scale, limit), limit) + caption + 8 * scale;
}
bool MediaPreviewController::openUrl(std::string_view url, std::uint64_t messageId) {
    auto &s = *impl_;
    const auto kind = urlKind(url);
    if (kind == PreviewKind::Link || s.mode(kind) == PreviewMode::Off)
        return false;
    bind(0, url, true, false);
    auto it = s.entries.find(0);
    if (it == s.entries.end())
        return false;
    s.directSourceId = messageId;
    s.activate(0);
    return true;
}
void MediaPreviewController::close() { impl_->close(true); }
bool MediaPreviewController::active() const { return impl_->overlay->visible(); }
bool MediaPreviewController::ready(std::uint64_t id) const {
    auto it = impl_->entries.find(id);
    return it != impl_->entries.end() && static_cast<bool>(it->second.asset);
}
bool MediaPreviewController::replacesUrl(std::uint64_t id) const {
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || impl_->mode(it->second.kind) == PreviewMode::Off)
        return false;
    const auto &entry = it->second;
    if (!entry.error.empty() || entry.pending || entry.needsConsent)
        return false;
    if (entry.kind == PreviewKind::Image)
        return entry.asset && entry.raster.width && entry.raster.height;
    if (entry.kind == PreviewKind::Audio || entry.kind == PreviewKind::Video)
        return static_cast<bool>(entry.asset);
    return entry.kind == PreviewKind::Link && entry.fetched;
}
std::string_view MediaPreviewController::error(std::uint64_t id) const {
    auto it = impl_->entries.find(id);
    return it == impl_->entries.end() ? std::string_view{} : it->second.error;
}
void MediaPreviewController::update(float width, float height, float scale) {
    auto &s = *impl_;
    if (s.policy != s.currentPolicy()) {
        invalidate();
        s.notify();
    }
    s.overlay->setSize(std::max(0.f, width), std::max(0.f, height));
    s.overlay->scale = std::max(.1f, scale);
    for (auto &result : s.service.poll()) {
        if (result.generation != s.generation)
            continue;
        auto ticket = s.tickets.find(result.id);
        if (ticket == s.tickets.end())
            continue;
        auto it = s.entries.find(ticket->second);
        s.tickets.erase(ticket);
        if (it == s.entries.end())
            continue;
        auto &entry = it->second;
        entry.pending = false;
        entry.ticket = 0;
        entry.fetched = true;
        entry.error = std::move(result.error);
        if (result.kind == PreviewKind::Unsupported && entry.error.empty())
            entry.error = "Unsupported preview content type.";
        if (!result.metadata.title.empty())
            entry.title = std::move(result.metadata.title);
        if (result.kind == PreviewKind::Link)
            entry.imageUrl = std::move(result.metadata.imageUrl);
        entry.asset = std::move(result.asset);
        entry.raster = std::move(result.raster);
        if (!result.metadata.description.empty())
            entry.description = std::move(result.metadata.description);
        entry.assetRevision = result.id;
        const auto previousKind = entry.requestedKind;
        if (result.kind != PreviewKind::Unsupported)
            entry.kind = result.kind;
        if (!entry.asset && entry.error.empty() && entry.kind != previousKind) {
            // A response classification is not consent to fetch its different type.
            s.request(it->first, entry, false);
            if (!entry.pending && entry.error.empty()) {
                entry.needsConsent = s.mode(entry.kind) != PreviewMode::Off;
                entry.error =
                    entry.needsConsent
                        ? "The server returned another content type. Choose Load to fetch it."
                        : "The server returned a content type disabled in preview settings.";
            }
        } else if (entry.kind == PreviewKind::Link && !entry.imageUrl.empty() &&
                   entry.automaticAllowed && s.mode(PreviewKind::Image) == PreviewMode::Automatic) {
            if (entry.metadataImages >= 4)
                entry.error = "Too many nested metadata images. Click to retry.";
            else {
                ++entry.metadataImages;
                if (entry.url != entry.imageUrl)
                    entry.attemptedKinds = 0;
                entry.url = entry.imageUrl;
                entry.kind = PreviewKind::Image;
                entry.fetched = false;
                s.request(it->first, entry, false);
            }
        }
        if (s.overlay->visible() && s.overlayId == it->first && !s.showingInfo) {
            if (entry.asset || entry.needsConsent)
                s.show(it->first, entry);
            else if (!entry.error.empty())
                s.overlay->status->setText(entry.error);
            else if (entry.kind == PreviewKind::Link && !entry.pending)
                s.info(it->first);
        }
        s.notify();
    }
    if (active()) {
        s.player.poll();
        if (!s.showingInfo &&
            (s.overlayKind == PreviewKind::Audio || s.overlayKind == PreviewKind::Video)) {
            auto it = s.entries.find(s.overlayId);
            if (it != s.entries.end() && s.player.loaded() && std::isfinite(s.player.duration()) &&
                s.player.duration() > 0)
                it->second.duration = s.player.duration();
            const auto fetchError = it == s.entries.end() ? std::string{} : it->second.error;
            s.overlay->status->setText(!fetchError.empty()         ? fetchError
                                       : !s.player.error().empty() ? s.player.error()
                                       : !s.player.loaded()
                                           ? "Loading media… playback requires Play."
                                           : clockText(s.player.position()) + " / " +
                                                 clockText(s.player.duration()) + " · Volume");
            s.overlay->play->setText(s.player.paused() ? "Play" : "Pause");
            s.overlay->play->setEnabled(s.player.ready());
            s.overlay->settingControls = true;
            s.overlay->seek->setEnabled(s.player.ready());
            s.overlay->seek->setRange(0, std::max(1., s.player.duration()));
            if (!s.overlay->seek->dragging())
                s.overlay->seek->setValue(s.player.position());
            if (!s.overlay->volume->dragging())
                s.overlay->volume->setValue(s.player.volume());
            s.overlay->settingControls = false;
        }
        s.overlay->markLayoutDirty();
    }
}
bool MediaPreviewController::handleKey(const InputArea::KeyData &key) {
    if (!active())
        return false;
    if (!key.preedit && key.pressed && key.sym == XKB_KEY_Escape) {
        close();
        return true;
    }
    if (!key.preedit && key.pressed && key.sym == XKB_KEY_Tab && impl_->dispatcher) {
        impl_->dispatcher->cycleTabFocusInSubtree(impl_->overlay,
                                                  (key.modifiers & KeyMod::Shift) != 0);
        return true;
    }
    if (impl_->dispatcher)
        impl_->dispatcher->keyEvent(key.sym, key.utf32, key.modifiers, key.pressed, key.preedit);
    return true;
}

struct MediaPreviewCard::Impl {
    MediaPreviewController &controller;
    std::uint64_t id = 0;
    Box *background;
    Image *image;
    InputArea *thumbnailAction;
    Label *status, *caption;
    std::function<void(std::string, std::uint64_t)> openLink;
    std::function<bool(std::uint64_t)> isReadable;
    LucideIcon *typeIcon;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    std::uint64_t shownRevision = 0;
    std::shared_ptr<MediaPreviewController::Impl::CardPin> visibleAsset;
    float scale = 1;
    explicit Impl(MediaPreviewController &c, Node &owner) : controller(c) {
        visibleAsset = std::make_shared<MediaPreviewController::Impl::CardPin>();
        visibleAsset->node = &owner;
        controller.impl_->cardPins.push_back(visibleAsset);
        background = child<Box>(owner);
        background->setHitTestVisible(false);
        image = child<Image>(owner);
        image->setFit(ImageFit::Contain);
        image->setHitTestVisible(false);
        thumbnailAction = child<InputArea>(owner);
        thumbnailAction->setFocusable(true);
        thumbnailAction->setTabStop(true);
        thumbnailAction->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
        thumbnailAction->setTooltip("Open preview; right-click for Info and Copy Link");
        thumbnailAction->setOnClick([this](const auto &pointer) {
            if (pointer.button == BTN_RIGHT)
                menu(pointer);
            else
                activate();
        });
        thumbnailAction->setOnKeyDown([this](const auto &key) {
            if (key.preedit)
                return;
            if (key.sym == XKB_KEY_Menu ||
                (key.sym == XKB_KEY_F10 && key.modifiers == KeyMod::Shift))
                menu({});
            else if (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_space)
                activate();
        });
        status = child<Label>(owner);
        status->setHitTestVisible(false);
        caption = child<Label>(owner);
        caption->setHitTestVisible(false);
        auto icon = std::make_unique<LucideIcon>("link");
        typeIcon = icon.get();
        typeIcon->setHitTestVisible(false);
        owner.addChild(std::move(icon));
    }
    bool readable(std::uint64_t message) {
        auto &state = *controller.impl_;
        if (state.policy != state.currentPolicy()) {
            controller.invalidate();
            return false;
        }
        if (isReadable && !isReadable(message)) {
            controller.bind(message, {}, false);
            return false;
        }
        const auto entry = state.entries.find(message);
        return entry != state.entries.end() && state.mode(entry->second.kind) != PreviewMode::Off;
    }
    void activate() {
        if (readable(id))
            controller.impl_->activate(id);
    }
    void menu(std::optional<InputArea::PointerData> pointer) {
        if (!readable(id))
            return;
        auto *context = thumbnailAction->popupContext();
        auto &state = *controller.impl_;
        auto entry = state.entries.find(id);
        if (!context || entry == state.entries.end())
            return;
        SelectPopupContext::DropdownRequest request;
        float left, top, right, bottom;
        Node::transformedBounds(thumbnailAction, left, top, right, bottom);
        request.anchorX = int(pointer ? pointer->sceneX : left);
        request.anchorY = int(pointer ? pointer->sceneY : top);
        request.anchorWidth = pointer ? 1 : std::max(1, int(right - left));
        request.anchorHeight = pointer ? 1 : std::max(1, int(bottom - top));
        request.pointerContextMenu = pointer.has_value();
        request.inputSerial =
            pointer ? std::optional<std::uint32_t>(pointer->serial) : std::nullopt;
        request.menuWidth = 200 * scale;
        request.fontSize = 14 * scale;
        ContextMenuEntry info;
        info.label = "Info";
        info.value = 0;
        ContextMenuEntry copy;
        copy.label = "Copy Link";
        copy.value = 1;
        copy.enabled = Input::textClipboard() != nullptr;
        request.menu = {info, copy};
        ContextMenuEntry open;
        open.label = "Open Link";
        open.value = 2;
        open.enabled = static_cast<bool>(openLink);
        request.menu.push_back(std::move(open));
        const auto generation = state.generation, message = id;
        const auto original = entry->second.original;
        std::weak_ptr<int> guard = alive;
        context->openSelectDropdown(
            request, {
                         .onSelect =
                             [this, guard, generation, message, original](std::size_t choice) {
                                 if (guard.expired())
                                     return;
                                 auto &state = *controller.impl_;
                                 auto entry = state.entries.find(message);
                                 if (generation != state.generation ||
                                     entry == state.entries.end() ||
                                     entry->second.original != original)
                                     return;
                                 if (!readable(message))
                                     return;
                                 if (choice == 0)
                                     state.info(message);
                                 else if (choice == 1 && Input::textClipboard())
                                     Input::textClipboard()->setClipboardText(original);
                                 else if (choice == 2 && openLink)
                                     openLink(original, message);
                             },
                         .onDismiss = {},
                     });
    }
};
MediaPreviewCard::MediaPreviewCard(MediaPreviewController &controller)
    : impl_(std::make_unique<Impl>(controller, *this)) {
    setClipChildren(true);
}
void MediaPreviewCard::setOpenLink(std::function<void(std::string, std::uint64_t)> callback) {
    impl_->openLink = std::move(callback);
}
void MediaPreviewCard::setReadable(std::function<bool(std::uint64_t)> callback) {
    impl_->isReadable = std::move(callback);
}
MediaPreviewCard::~MediaPreviewCard() = default;
void MediaPreviewCard::bind(std::uint64_t id, std::string_view url, bool readable,
                            bool automaticAllowed) {
    if (impl_->id != id || !readable) {
        impl_->shownRevision = 0;
        impl_->visibleAsset->asset.reset();
    }
    impl_->visibleAsset->id = id;
    impl_->id = id;
    impl_->thumbnailAction->setTabFocusKey("blastirc.media.thumbnail." + std::to_string(id));
    impl_->controller.bind(id, url, readable, automaticAllowed);
    markLayoutDirty();
}
float MediaPreviewCard::measure(Renderer &renderer, float width, float scale) const {
    return impl_->controller.measure(renderer, impl_->id, width, scale);
}
void MediaPreviewCard::sync(Renderer &renderer, float width, float scale) {
    impl_->scale = scale;
    setSize(width, measure(renderer, width, scale));
    setVisible(height() > 0);
    setHitTestVisible(height() > 0);
    if (!visible())
        impl_->visibleAsset->asset.reset();
    markLayoutDirty();
}
void MediaPreviewCard::doLayout(Renderer &renderer) {
    auto &s = *impl_;
    auto &controller = *s.controller.impl_;
    auto it = controller.entries.find(s.id);
    if (it == controller.entries.end())
        return;
    auto &entry = it->second;
    const float scale = s.scale;
    const bool replacing = s.controller.replacesUrl(s.id);
    const float captionHeight =
        replacing ? controller.captionHeight(renderer, entry, width(), scale) : 0;
    const float h = std::max(0.f, height() - 8 * scale - captionHeight);
    const float w = std::min(width(), 320 * scale);
    s.visibleAsset->asset = entry.asset;
    entry.lastUse = ++controller.clock;
    s.background->setSize(w, h + captionHeight);
    s.background->setFill(themeSurfaceColor("surface_container"));
    s.background->setRadius(themeCornerRadius());
    if (entry.asset && entry.kind == PreviewKind::Image && s.shownRevision != entry.assetRevision) {
        s.image->clear(renderer);
        if (!loadRaster(*s.image, renderer, *entry.asset,
                        std::clamp(static_cast<int>(240 * scale), 64, 512)))
            entry.error = "Image decode failed.";
        s.shownRevision = entry.assetRevision;
    }
    const bool thumb = entry.kind == PreviewKind::Image && entry.asset && entry.raster.width &&
                       entry.raster.height && entry.error.empty();
    s.image->setVisible(thumb);
    s.image->setPosition(0, 0);
    s.image->setSize(w, h);
    s.thumbnailAction->setPosition(0, 0);
    s.thumbnailAction->setSize(w, h + captionHeight);
    labelStyle(*s.status, scale);
    s.status->setMaxLines(4);
    s.status->setMaxWidth(std::max(0.f, w - 16 * scale));
    s.status->setPosition(8 * scale, 8 * scale);
    const bool contentTile = !thumb && replacing;
    s.typeIcon->setVisible(contentTile);
    s.typeIcon->setName(entry.kind == PreviewKind::Audio   ? "music"
                        : entry.kind == PreviewKind::Video ? "video"
                                                           : "link");
    s.typeIcon->setColor(themeForegroundColor("on_surface"));
    s.typeIcon->setPosition(8 * scale, 8 * scale);
    s.typeIcon->setSize(24 * scale, 24 * scale);
    if (contentTile)
        s.status->setPosition(8 * scale, 40 * scale);
    s.status->setVisible(!thumb);
    s.status->setText(!entry.error.empty() ? entry.error
                      : entry.pending      ? "Loading preview…"
                      : entry.asset        ? (entry.kind == PreviewKind::Audio ? "Audio" : "Video")
                      : entry.fetched && entry.kind == PreviewKind::Link
                          ? (entry.imageUrl.empty() ? "No thumbnail available"
                             : controller.mode(PreviewKind::Image) == PreviewMode::Off
                                 ? "Web page · Thumbnail disabled"
                                 : "Web page · Load thumbnail")
                          : "Load preview");
    controller.prepareCaption(*s.caption, entry, width(), scale);
    s.caption->setPosition(8 * scale, h + 4 * scale);
    s.caption->setVisible(replacing);
    Node::doLayout(renderer);
}
} // namespace blast
