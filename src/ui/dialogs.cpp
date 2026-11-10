#include "dialogs.hpp"

#include "../irc/protocol.hpp"
#include "../model.hpp"
#include "../spellcheck.hpp"
#include "../storage.hpp"
#include "../theme/theme.hpp"
#include "channel_avatar.hpp"
#include "logic.hpp"
#include "transcript.hpp"
#include "ui.hpp"

#include <core/input/key_modifiers.h>
#include <core/input/keybind_matcher.h>
#include <fontconfig/fontconfig.h>
#include <render/scene/input_dispatcher.h>
#include <ui/controls/box.h>
#include <ui/controls/button.h>
#include <ui/controls/flex.h>
#include <ui/controls/input.h>
#include <ui/controls/label.h>
#include <ui/controls/roving_list_nav.h>
#include <ui/controls/scroll_view.h>
#include <ui/controls/select.h>
#include <ui/controls/slider.h>
#include <ui/controls/toggle.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace blast {
namespace {
std::vector<std::string> words(std::string text) {
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream stream(text);
    std::vector<std::string> result;
    for (std::string word; stream >> word;)
        result.push_back(std::move(word));
    return result;
}

template <class Range> std::string joined(const Range &values) {
    std::string result;
    for (const auto &value : values) {
        if (!result.empty())
            result += ", ";
        result += value;
    }
    return result;
}

std::vector<std::string> installedFonts() {
    std::vector<std::string> result;
    FcPattern *pattern = FcPatternCreate();
    FcObjectSet *objects = FcObjectSetBuild(FC_FAMILY, nullptr);
    if (pattern && objects) {
        if (FcFontSet *fonts = FcFontList(nullptr, pattern, objects)) {
            for (int i = 0; i < fonts->nfont; ++i) {
                FcChar8 *family = nullptr;
                for (int n = 0;
                     FcPatternGetString(fonts->fonts[i], FC_FAMILY, n, &family) == FcResultMatch;
                     ++n)
                    result.emplace_back(reinterpret_cast<const char *>(family));
            }
            FcFontSetDestroy(fonts);
        }
    }
    if (objects)
        FcObjectSetDestroy(objects);
    if (pattern)
        FcPatternDestroy(pattern);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool colorValid(std::string_view color) {
    if (color.size() != 7 || color.front() != '#')
        return false;
    return std::all_of(color.begin() + 1, color.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// Choose the responsive orientation inside native layout, before the enclosing
// ScrollView resolves a reveal request against the resulting control positions.
class BindingRow final : public Flex {
    void adapt(float availableWidth) {
        const auto *current = static_cast<const Flex *>(children()[0].get());
        const auto *actions = static_cast<const Flex *>(children()[1].get());
        // Non-wrapping Flex divides positive grow shares equally before min clamping.
        const bool stacked =
            availableWidth < 2 * std::max(current->minWidth(), actions->minWidth()) + gap();
        setDirection(stacked ? FlexDirection::Vertical : FlexDirection::Horizontal);
        setAlign(stacked ? FlexAlign::Stretch : FlexAlign::End);
    }

    LayoutSize doMeasure(Renderer &renderer, const LayoutConstraints &constraints) override {
        if (constraints.hasMaxWidth)
            adapt(constraints.maxWidth);
        return Flex::doMeasure(renderer, constraints);
    }

    void doArrange(Renderer &renderer, const LayoutRect &rect) override {
        adapt(rect.width);
        Flex::doArrange(renderer, rect);
    }
};

void refreshFonts(Node &node) {
    if (auto *input = dynamic_cast<Input *>(&node)) {
        input->setFontFamily(themeUiFont().family);
        input->setFontFeatures(themeUiFont().features);
    } else if (auto *label = dynamic_cast<Label *>(&node)) {
        label->setFontFamily(themeUiFont().family);
        label->setFontFeatures(themeUiFont().features);
    }
    for (const auto &child : node.children())
        refreshFonts(*child);
}
} // namespace

struct Dialogs::Impl {
    enum class Tab { Appearance, Keys, Chat, Fx, Sounds, Servers, Identities };
    static constexpr const char *tabNames[] = {"Appearance", "Keyboard", "Chat",      "Effects",
                                               "Sounds",     "Networks", "Identities"};
    enum class MetricKind {
        Flex,
        Section,
        Label,
        Button,
        Input,
        ProfileInput,
        Toggle,
        Slider,
        Select,
        Scroll,
        Panel
    };
    struct Metric {
        Node *node;
        MetricKind kind;
        float font = Style::fontSizeBody, gap = 0, top = 0, right = 0, bottom = 0, left = 0;
        float minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;
    };
    Dialogs &owner;
    Model &model;
    std::function<void(std::string)> execute;
    InputDispatcher *dispatcher = nullptr;
    InputDispatcher::TabFocusSnapshot returnFocus;
    InputArea *confirmReturnFocus = nullptr;
    InputArea *blocker = nullptr;
    Box *dim = nullptr;
    Box *panel = nullptr;
    Flex *chrome = nullptr;
    ScrollView *scroll = nullptr;
    RovingListNavHost *categoryNavigation = nullptr;
    ScrollView *navigation = nullptr;
    Input *settingsSearch = nullptr;
    Select *settingsResults = nullptr;
    std::string settingsQuery, currentSection;
    struct SearchTarget {
        Tab category;
        std::string caption, terms;
        Node *node;
    };
    std::vector<SearchTarget> searchTargets;
    std::vector<std::size_t> rankedTargets;
    std::array<Flex *, 7> categoryPages{};
    std::map<std::string, Input *> bindingFields;
    Label *bindingConflicts = nullptr;
    std::map<std::string, std::string> lastConflictBindings;
    Select *categoryPicker = nullptr;
    Label *detailTitle = nullptr;
    Label *topicTitle = nullptr;
    struct ProfileCard {
        Flex *body = nullptr;
        Label *nick = nullptr, *avatar = nullptr;
        std::array<Flex *, 5> groups{};
        std::array<Input *, 12> fields{};
    };
    std::vector<ProfileCard> profileCards;
    Input *profileLoading = nullptr, *profileError = nullptr;
    std::string profileRequest;
    std::uint64_t profileRevision = 0;
    Button *recordingButton = nullptr;
    Input *recordingField = nullptr;
    std::string recordingName, recordedStrokes, originalChord;
    Flex *page = nullptr;
    Label *feedback = nullptr;
    Label *applicationStatus = nullptr;
    Label *motionStatus = nullptr;
    Label *soundStatus = nullptr;
    Label *cursorStatus = nullptr;
    Label *identityScope = nullptr;
    Button *connectButton = nullptr;
    Button *saveButton = nullptr;
    std::string lastApplicationStatus;
    std::uint64_t pendingSaveRequest = 0;
    bool closeAfterSave = false;
    bool saveFeedbackActive = false;
    Label *themeStatus = nullptr;
    Select *networkPicker = nullptr;
    Select *identityPicker = nullptr;
    Select *networkIdentityPicker = nullptr;
    std::vector<std::pair<Label *, std::size_t>> identityScopes;
    struct SharedIdentityField {
        Input *control;
        std::size_t index;
        std::string Identity::*member;
    };
    std::vector<SharedIdentityField> sharedIdentityFields;
    Box *confirmPanel = nullptr;
    Flex *confirmChrome = nullptr;
    Label *confirmTitle = nullptr;
    Label *confirmMessage = nullptr;
    Button *confirmAccept = nullptr;
    Button *confirmCancel = nullptr;
    std::function<void()> confirmAction;
    std::string mode, confirmation, nick, searchQuery, searchBuffer;
    std::string topicBuffer, editedTopic;
    std::string configMessage, recoveryMessage, fontFamily, fontFeatures;
    std::string lastThemeSource, lastGeometry, lastDialog;
    Tab tab = Tab::Appearance;
    std::size_t serverIndex = 0, identityIndex = 0;
    std::string sojuEditId;
    std::string sojuEditParent;
    std::shared_ptr<std::string> displayedServer;
    std::map<std::string, std::string> sojuEditAttributes, sojuEditChanges;
    Label *sojuStatus = nullptr;
    Label *connectionStatus = nullptr;
    bool sojuEditMode = false;
    Button *sojuSubmit = nullptr;
    std::vector<Button *> sojuMutationButtons;
    struct NetworkOption {
        std::string key, caption, parentCaption;
    };
    std::vector<NetworkOption> networkOptions;
    bool fontsLoaded = false;
    float zoom = 1;
    float appliedZoom = 0, appliedCorners = -1;
    bool metricsDirty = true;
    float geometryX = -1, geometryY = -1, geometryWidth = -1, geometryHeight = -1;
    std::vector<std::string> fonts;
    std::vector<std::pair<Tab, Button *>> tabButtons;
    std::set<Input *> invalidFields;
    std::vector<Metric> metrics;
    // Detached controls survive their own callbacks; destruction happens after native dispatch.
    std::vector<std::unique_ptr<Node>> retired;

    Impl(Dialogs &owner_, Model &model_, std::function<void(std::string)> execute_)
        : owner(owner_), model(model_), execute(std::move(execute_)) {
        auto shade = std::make_unique<Box>();
        dim = shade.get();
        dim->setFill(rgba(0, 0, 0, .58f));
        dim->setHitTestVisible(false);
        owner.addChild(std::move(shade));
        auto catchInput = std::make_unique<InputArea>();
        blocker = catchInput.get();
        blocker->setFocusable(true);
        blocker->setTabStop(false);
        blocker->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}));
        blocker->setOnAxisHandler([](const InputArea::PointerData &) { return true; });
        owner.addChild(std::move(catchInput));
        makeConfirmation();
        owner.setVisible(false);
        owner.setHitTestVisible(false);
        owner.setParticipatesInLayout(false);
        owner.setExcludeSubtreeFromTabOrder(true);
        captureStartupMessages();
        if (auto found = model.settings.workspace.find("native_dialog");
            found != model.settings.workspace.end())
            lastDialog = found->second;
        if (auto found = model.settings.workspace.find("native_dialog_rect");
            found != model.settings.workspace.end())
            lastGeometry = found->second;
        if (!configMessage.empty())
            open("config-error");
        else if (!recoveryMessage.empty())
            open("recovery");
        else if (model.firstRun)
            open("first-run");
    }

    void captureStartupMessages() {
        auto captureConfig = [this](const std::string &text) {
            if (text.starts_with("Configuration was not loaded;") ||
                text.starts_with("Configuration errors must be fixed before saving;"))
                configMessage = text;
        };
        captureConfig(model.status);
        for (const auto &[id, buffer] : model.buffers) {
            (void)id;
            for (const auto &message : buffer.messages) {
                if (message.nick != "BlastIRC" || !message.presence ||
                    !message.id.starts_with("local:"))
                    continue;
                if (buffer.kind == "server")
                    captureConfig(message.text);
                if (!message.history &&
                    message.text.starts_with("Send not confirmed; text restored to draft — "
                                             "Previous session ended before confirmation")) {
                    if (!recoveryMessage.empty())
                        recoveryMessage += '\n';
                    recoveryMessage += buffer.title + ": " + message.text;
                }
            }
        }
        if (recoveryMessage.empty() &&
            model.status == "Send failed: Previous session ended before confirmation")
            recoveryMessage = model.status;
    }

    void track(Node *node, MetricKind kind) {
        Metric metric{node, kind};
        if (auto *flex = dynamic_cast<Flex *>(node)) {
            metric.gap = flex->gap();
            metric.top = flex->paddingTop();
            metric.right = flex->paddingRight();
            metric.bottom = flex->paddingBottom();
            metric.left = flex->paddingLeft();
            metric.minWidth = flex->minWidth();
            metric.minHeight = flex->minHeight();
            metric.maxWidth = flex->maxWidth();
            metric.maxHeight = flex->maxHeight();
        }
        if (kind == MetricKind::Label)
            metric.font = static_cast<Label *>(node)->fontSize();
        else if (kind == MetricKind::Button)
            metric.font = static_cast<Button *>(node)->label()->fontSize();
        metrics.push_back(metric);
        metricsDirty = true;
    }

    void padding(Flex &node, float value) {
        node.setPadding(value);
        for (auto &metric : metrics)
            if (metric.node == &node) {
                metric.top = metric.right = metric.bottom = metric.left = value;
                metricsDirty = true;
                return;
            }
    }

    void forgetMetrics(Node *subtree) {
        std::erase_if(metrics, [subtree](const Metric &metric) {
            for (Node *node = metric.node; node; node = node->parent())
                if (node == subtree)
                    return true;
            return false;
        });
    }

    void applyMetrics() {
        const float corners = themeCornerRadius();
        if (!metricsDirty && appliedZoom == zoom && appliedCorners == corners)
            return;
        for (const auto &metric : metrics) {
            if (auto *flex = dynamic_cast<Flex *>(metric.node)) {
                flex->setGap(metric.gap * zoom);
                flex->setPadding(metric.top * zoom, metric.right * zoom, metric.bottom * zoom,
                                 metric.left * zoom);
                flex->setMinWidth(metric.minWidth * zoom);
                flex->setMinHeight(metric.minHeight * zoom);
                flex->setMaxWidth(metric.maxWidth * zoom);
                flex->setMaxHeight(metric.maxHeight * zoom);
            }
            switch (metric.kind) {
            case MetricKind::Flex:
                break;
            case MetricKind::Section:
                static_cast<Flex *>(metric.node)->setCardStyle(zoom);
                break;
            case MetricKind::Label:
                static_cast<Label *>(metric.node)->setFontSize(metric.font * zoom);
                break;
            case MetricKind::Button: {
                auto *control = static_cast<Button *>(metric.node);
                control->setFontSize(metric.font * zoom);
                control->setRadius(corners);
                control->setControlHeight(std::max(32.f, Style::controlHeight * zoom));
                break;
            }
            case MetricKind::Input:
            case MetricKind::ProfileInput: {
                auto *control = static_cast<Input *>(metric.node);
                control->setFontSize(Style::fontSizeBody * zoom);
                control->setControlHeight(std::max(32.f, Style::controlHeight * zoom));
                control->setHorizontalPadding(Style::spaceMd * zoom);
                control->setFrameRadius(corners);
                if (metric.kind == MetricKind::ProfileInput)
                    control->setSize(control->width(), 72.f * zoom);
                break;
            }
            case MetricKind::Toggle:
                // Native Toggle::setScale changes track/thumb metrics, not Node's transform.
                static_cast<Toggle *>(metric.node)
                    ->setScale(std::max(
                        32.f / (Style::toggleThumbSizeMd + 2 * Style::toggleInsetMd), zoom));
                break;
            case MetricKind::Slider: {
                auto *control = static_cast<Slider *>(metric.node);
                control->setTrackHeight(Style::sliderTrackHeight * zoom);
                control->setThumbSize(Style::sliderThumbSize * zoom);
                control->setControlHeight(std::max(32.f, Style::controlHeight * zoom));
                break;
            }
            case MetricKind::Select: {
                auto *control = static_cast<Select *>(metric.node);
                control->setFontSize(Style::fontSizeBody * zoom);
                control->setControlHeight(std::max(32.f, Style::controlHeight * zoom));
                control->setHorizontalPadding(Style::spaceMd * zoom);
                control->setGlyphSize(14 * zoom);
                control->setRadius(corners);
                break;
            }
            case MetricKind::Scroll: {
                auto *control = static_cast<ScrollView *>(metric.node);
                control->setContentScale(zoom);
                control->setRadius(corners);
                break;
            }
            case MetricKind::Panel: {
                auto *control = static_cast<Box *>(metric.node);
                control->setRadius(corners);
                control->setBorder(colorSpecFromRole(ColorRole::OutlineVariant),
                                   Style::borderWidth * zoom);
                break;
            }
            }
        }
        appliedZoom = zoom;
        appliedCorners = corners;
        metricsDirty = false;
    }

    std::unique_ptr<Flex> column(float gap = 10, MetricKind kind = MetricKind::Flex) {
        auto result = std::make_unique<Flex>();
        result->setDirection(FlexDirection::Vertical);
        result->setAlign(FlexAlign::Stretch);
        result->setFillWidth(true);
        result->setGap(gap);
        track(result.get(), kind);
        return result;
    }

    Flex *row(Flex &parent, bool wrap = true) {
        auto result = std::make_unique<Flex>();
        auto *ptr = result.get();
        result->setDirection(FlexDirection::Horizontal);
        result->setAlign(FlexAlign::Center);
        result->setFillWidth(true);
        result->setGap(8);
        result->setWrap(wrap);
        track(ptr, MetricKind::Flex);
        parent.addChild(std::move(result));
        return ptr;
    }

    Label *label(Flex &parent, std::string_view text, bool heading = false) {
        auto result = std::make_unique<Label>();
        auto *ptr = result.get();
        result->setText(text);
        result->setUseMarkup(false);
        result->setMaxLines(0);
        result->setFontSize(heading ? 18 : 14);
        result->setFontFamily(themeUiFont().family);
        result->setFontFeatures(themeUiFont().features);
        result->setColor(colorSpecFromRole(
            mode == "settings" && !heading ? ColorRole::OnSurfaceVariant : ColorRole::OnSurface));
        track(ptr, MetricKind::Label);
        if (heading)
            result->setFontWeight(FontWeight::Bold);
        parent.addChild(std::move(result));
        return ptr;
    }

    Button *button(Flex &parent, std::string_view text, std::function<void()> callback,
                   ButtonVariant variant = ButtonVariant::Default) {
        auto result = std::make_unique<Button>();
        auto *ptr = result.get();
        result->setText(text);
        result->setVariant(variant);
        result->setOnClick(std::move(callback));
        track(ptr, MetricKind::Button);
        parent.addChild(std::move(result));
        return ptr;
    }

    void indexSetting(std::string_view name, Node *node) {
        if (mode != "settings" || !page)
            return;
        const std::string category = tabNames[std::size_t(tab)];
        searchTargets.push_back({tab, category + " · " + currentSection + " · " + std::string(name),
                                 std::string(name) + " " + currentSection + " " + category, node});
    }

    void jumpToSetting(std::size_t result) {
        if (result >= rankedTargets.size())
            return;
        const auto target = searchTargets[rankedTargets[result]];
        chooseTab(target.category);
        // Search remains ready for refinement; Tab reaches the ranked result picker.
        if (dispatcher)
            dispatcher->setFocus(settingsSearch->inputArea());
        scroll->requestScrollIntoView(target.node);
    }

    void filterSettings() {
        if (!settingsResults)
            return;
        rankedTargets.clear();
        if (!settingsQuery.empty()) {
            for (std::size_t i = 0; i < searchTargets.size(); ++i)
                if (ui_logic::fuzzy(settingsQuery, searchTargets[i].terms) > -100000)
                    rankedTargets.push_back(i);
            std::stable_sort(rankedTargets.begin(), rankedTargets.end(), [this](auto a, auto b) {
                return ui_logic::fuzzy(settingsQuery, searchTargets[a].terms) >
                       ui_logic::fuzzy(settingsQuery, searchTargets[b].terms);
            });
        }
        std::vector<std::string> captions;
        for (auto index : rankedTargets)
            captions.push_back(searchTargets[index].caption);
        settingsResults->setOptions(std::move(captions));
        settingsResults->clearSelection();
        settingsResults->setPlaceholder(rankedTargets.empty() ? "No matching settings"
                                                              : "Choose a setting…");
        settingsResults->setVisible(!settingsQuery.empty());
        settingsResults->setParticipatesInLayout(!settingsQuery.empty());
    }

    Flex *section(std::string_view title, std::string_view description = {}) {
        auto group = column(12, MetricKind::Section);
        auto *body = group.get();
        currentSection = title;
        indexSetting(title, body);
        label(*body, title, true);
        if (!description.empty())
            label(*body, description)->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        auto separator = std::make_unique<Flex>();
        separator->setFillWidth(true);
        separator->setMinHeight(1);
        separator->setMaxHeight(1);
        separator->setFill(colorSpecFromRole(ColorRole::OutlineVariant));
        separator->setHitTestVisible(false);
        track(separator.get(), MetricKind::Flex);
        body->addChild(std::move(separator));
        page->addChild(std::move(group));
        return body;
    }

    Input *text(Flex &parent, std::string_view name, std::string_view value,
                std::function<void(const std::string &)> changed, bool password = false) {
        auto group = column(4);
        label(*group, name)->setColor(colorSpecFromRole(ColorRole::OnSurface));
        auto result = std::make_unique<Input>();
        auto *ptr = result.get();
        result->setValue(value);
        result->setPasswordMode(password);
        result->setFontFamily(themeUiFont().family);
        result->setFontFeatures(themeUiFont().features);
        result->setClearButtonEnabled(!password);
        result->setMinLayoutWidth(0);
        result->setFlexGrow(1);
        result->setOnChange(std::move(changed));
        result->setOnFocusGain([this, unit = group.get()] {
            if (recordingButton)
                stopRecording(false);
            if (scroll)
                scroll->requestScrollIntoView(unit);
        });
        track(ptr, MetricKind::Input);
        indexSetting(name, ptr);
        group->addChild(std::move(result));
        parent.addChild(std::move(group));
        return ptr;
    }

    void integer(Flex &parent, std::string_view name, int value, int low, int high,
                 std::function<void(int)> changed) {
        auto *field = text(parent, name, std::to_string(value), {});
        field->setOnChange(
            [this, field, low, high, changed = std::move(changed)](const std::string &s) {
                int number = 0;
                const auto parsed = std::from_chars(s.data(), s.data() + s.size(), number);
                const bool valid = parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size() &&
                                   number >= low && number <= high;
                field->setInvalid(!valid);
                if (valid) {
                    invalidFields.erase(field);
                    changed(number);
                } else {
                    invalidFields.insert(field);
                }
            });
    }

    Toggle *toggle(Flex &parent, std::string_view name, bool value,
                   std::function<void(bool)> changed) {
        auto *line = row(parent, false);
        auto *title = label(*line, name);
        title->setColor(colorSpecFromRole(ColorRole::OnSurface));
        title->setFlexGrow(1);
        auto result = std::make_unique<Toggle>();
        auto *ptr = result.get();
        result->setCheckedImmediate(value);
        result->setOnChange(std::move(changed));
        track(ptr, MetricKind::Toggle);
        indexSetting(name, ptr);
        line->addChild(std::move(result));
        return ptr;
    }

    Slider *slider(Flex &parent, std::string_view name, float value, double low, double high,
                   double step, std::function<void(float)> changed) {
        auto group = column(3);
        auto *line = row(*group, false);
        auto *title = label(*line, name);
        title->setColor(colorSpecFromRole(ColorRole::OnSurface));
        title->setFlexGrow(1);
        char display[40];
        std::snprintf(display, sizeof(display), "%.2f", double(value));
        auto *number = label(*line, display);
        number->setColor(colorSpecFromRole(ColorRole::OnSurface));
        number->setTextAlign(TextAlign::End);
        auto result = std::make_unique<Slider>();
        auto *ptr = result.get();
        result->setWheelAdjustEnabled(false);
        result->setRange(low, high);
        result->setStep(step);
        result->setValue(value);
        result->setFillWidth(true);
        result->setOnValueChanged([number, changed = std::move(changed)](double next) {
            changed(float(next));
            char display[40];
            std::snprintf(display, sizeof(display), "%.2f", next);
            number->setText(display);
        });
        track(ptr, MetricKind::Slider);
        indexSetting(name, ptr);
        group->addChild(std::move(result));
        parent.addChild(std::move(group));
        return ptr;
    }

    Select *select(Flex &parent, std::string_view name, std::vector<std::string> options,
                   std::string_view selected,
                   std::function<void(std::size_t, std::string_view)> changed) {
        auto group = column(4);
        label(*group, name)->setColor(colorSpecFromRole(ColorRole::OnSurface));
        auto result = std::make_unique<Select>();
        auto *ptr = result.get();
        const auto found = std::find(options.begin(), options.end(), selected);
        const auto index = std::size_t(found - options.begin());
        const bool hasSelection = found != options.end();
        result->setOptions(std::move(options));
        if (hasSelection)
            result->setSelectedIndexSilently(index);
        else {
            result->clearSelection();
            result->setPlaceholder(selected.empty() ? "Choose…" : selected);
        }
        result->setFillWidth(true);
        result->setOnSelectionChanged(std::move(changed));
        track(ptr, MetricKind::Select);
        indexSetting(name, ptr);
        group->addChild(std::move(result));
        parent.addChild(std::move(group));
        return ptr;
    }

    void setFeedback(std::string_view message, bool error = true) {
        if (feedback) {
            feedback->setText(message);
            feedback->setColor(
                colorSpecFromRole(error ? ColorRole::Error : ColorRole::OnSurfaceVariant));
            feedback->setVisible(!message.empty());
            feedback->setParticipatesInLayout(!message.empty());
            if (!message.empty() && scroll)
                scroll->requestScrollIntoView(feedback);
        }
    }

    void detach(Node *child, Node &parent) {
        if (!child)
            return;
        if (dispatcher)
            for (Node *node = dispatcher->focusedArea(); node; node = node->parent())
                if (node == child) {
                    dispatcher->setFocus(nullptr);
                    break;
                }
        forgetMetrics(child);
        child->setVisible(false);
        retired.push_back(parent.removeChild(child));
    }

    void showOverlay() {
        owner.setVisible(true);
        owner.setHitTestVisible(true);
        owner.setExcludeSubtreeFromTabOrder(false);
    }

    void focusFirst() {
        if (!dispatcher)
            return;
        if (!confirmation.empty()) {
            dispatcher->setFocus(confirmCancel->inputArea());
            return;
        }
        auto *target = dispatcher->firstTabFocusUnder(page ? static_cast<Node *>(page) : panel);
        if (!target)
            target = dispatcher->firstTabFocusUnder(panel);
        dispatcher->setFocus(target);
        if (scroll && target)
            scroll->requestScrollIntoView(target);
        if (!dispatcher->focusedArea())
            dispatcher->setFocus(blocker);
    }

    void chooseTab(Tab next) {
        if (tab == next)
            return;
        tab = next;
        for (const auto &[candidate, control] : tabButtons)
            control->setVariant(candidate == tab ? ButtonVariant::TabActive : ButtonVariant::Tab);
        if (categoryPicker)
            categoryPicker->setSelectedIndexSilently(std::size_t(tab));
        if (detailTitle)
            detailTitle->setText(std::string("Settings · ") + tabNames[std::size_t(tab)]);
        if (categoryNavigation)
            categoryNavigation->notifyExternalSelectionChanged();
        if (categoryPages[std::size_t(tab)]) {
            stopRecording(false);
            for (std::size_t i = 0; i < categoryPages.size(); ++i) {
                auto *body = categoryPages[i];
                if (!body)
                    continue;
                const bool active = i == std::size_t(tab);
                body->setVisible(active);
                body->setParticipatesInLayout(active);
                body->setExcludeSubtreeFromTabOrder(!active);
            }
            scroll->setScrollOffset(0);
        } else {
            rebuildPage(false);
        }
    }

    void stopRecording(bool commit) {
        auto *control = std::exchange(recordingButton, nullptr);
        if (!control)
            return;
        control->inputArea()->setCapturesAllKeys(false);
        control->setText("Record");
        control->setVariant(ButtonVariant::Default);
        if (commit && !recordedStrokes.empty()) {
            model.settings.bindings[recordingName] = recordedStrokes;
            recordingField->setValue(recordedStrokes);
        } else {
            recordingField->setValue(originalChord);
        }
        recordingField = nullptr;
        recordingName.clear();
        recordedStrokes.clear();
        originalChord.clear();
    }

    void startRecording(Button *control, Input *field, const std::string &name) {
        stopRecording(false);
        recordingButton = control;
        recordingField = field;
        recordingName = name;
        originalChord = field->value();
        control->setText("Finish");
        control->setVariant(ButtonVariant::Primary);
        control->inputArea()->setCapturesAllKeys(true);
        if (dispatcher)
            dispatcher->setFocus(control->inputArea());
        if (scroll)
            scroll->requestScrollIntoView(control);
    }

    void recordKey(const InputArea::KeyData &key) {
        if (!recordingButton || !key.pressed || key.preedit)
            return;
        const auto modifiers = key.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super);
        if (modifiers == KeyMod::Ctrl && (key.sym == XKB_KEY_g || key.sym == XKB_KEY_G)) {
            stopRecording(false);
            return;
        }
        if (modifiers == KeyMod::Ctrl &&
            (key.sym == XKB_KEY_Return || key.sym == XKB_KEY_KP_Enter)) {
            stopRecording(true);
            return;
        }
        if (auto stroke = bindingStroke(key); !stroke.empty()) {
            if (!recordedStrokes.empty())
                recordedStrokes += ' ';
            recordedStrokes += stroke;
            recordingField->setValue(recordedStrokes);
        }
    }

    void beginPanel(std::string name, std::string_view title, bool tabs) {
        if (mode == "whois" && name != "whois") {
            model.whoisProfile.open = false;
            model.whoisProfile.loading = false;
            ++model.whoisProfile.revision;
        }
        profileCards.clear();
        profileLoading = profileError = nullptr;
        if (mode.empty() && confirmation.empty() && dispatcher)
            returnFocus = dispatcher->captureTabFocus();
        hideConfirmation();
        stopRecording(false);
        detach(panel, owner);
        panel = nullptr;
        page = nullptr;
        categoryNavigation = nullptr;
        navigation = nullptr;
        categoryPicker = nullptr;
        detailTitle = nullptr;
        settingsSearch = nullptr;
        settingsResults = nullptr;
        searchTargets.clear();
        categoryPages.fill(nullptr);
        bindingFields.clear();
        bindingConflicts = nullptr;
        topicTitle = nullptr;
        tabButtons.clear();
        feedback = themeStatus = applicationStatus = motionStatus = soundStatus = cursorStatus =
            identityScope = connectionStatus = nullptr;
        connectButton = saveButton = nullptr;
        saveFeedbackActive = false;
        networkPicker = identityPicker = nullptr;
        invalidFields.clear();
        mode = std::move(name);
        auto background = std::make_unique<Box>();
        panel = background.get();
        panel->setDialogStyle();
        track(panel, MetricKind::Panel);
        panel->setClipChildren(true);
        panel->setZIndex(2);
        auto layout = column(tabs ? 8 : 10);
        chrome = layout.get();
        padding(*chrome, tabs ? 12 : 18);
        auto *header = row(*chrome, false);
        auto *heading = label(*header,
                              tabs ? std::string("Settings · ") + tabNames[std::size_t(tab)]
                                   : std::string(title),
                              true);
        heading->setFlexGrow(1);
        if (tabs)
            detailTitle = heading;
        if (!tabs)
            button(*header, "Close", [this] { dismiss(); }, ButtonVariant::Ghost);
        if (tabs) {
            settingsSearch =
                text(*chrome, "Search settings", settingsQuery, [this](const std::string &value) {
                    settingsQuery = value;
                    filterSettings();
                });
            settingsSearch->inputArea()->setTabFocusKey("blastirc.settings.search");
            settingsSearch->setOnSubmit([this](const std::string &) { jumpToSetting(0); });
            settingsResults =
                select(*chrome, "Matching settings", {}, "",
                       [this](std::size_t index, std::string_view) { jumpToSetting(index); });
        }
        auto viewport = std::make_unique<ScrollView>();
        scroll = viewport.get();
        track(scroll, MetricKind::Scroll);
        scroll->setFlexGrow(1);
        scroll->setFillWidth(true);
        scroll->setFillHeight(true);
        scroll->setReserveScrollbarGutter(true);
        scroll->setViewportPaddingH(0);
        scroll->setViewportPaddingV(0);
        scroll->content()->setDirection(FlexDirection::Vertical);
        scroll->content()->setAlign(FlexAlign::Stretch);
        scroll->content()->setGap(10);
        track(scroll->content(), MetricKind::Flex);
        if (tabs) {
            auto compact = std::make_unique<Select>();
            categoryPicker = compact.get();
            std::vector<std::string> names(std::begin(tabNames), std::end(tabNames));
            compact->setOptions(std::move(names));
            compact->setOptionIcons(
                {"palette", "keyboard", "messages", "sparkles", "volume-2", "network", "user"});
            compact->setSelectedIndexSilently(std::size_t(tab));
            compact->setOnSelectionChanged([this](std::size_t index, std::string_view) {
                if (index < std::size(tabNames))
                    chooseTab(Tab(index));
            });
            compact->setFillWidth(true);
            compact->setVisible(false);
            track(compact.get(), MetricKind::Select);
            header->addChild(std::move(compact));

            auto body = column(8);
            body->setDirection(FlexDirection::Horizontal);
            body->setFillHeight(true);
            body->setFlexGrow(1);
            auto sidebar = std::make_unique<ScrollView>();
            navigation = sidebar.get();
            sidebar->setFillHeight(true);
            sidebar->setMinWidth(200);
            sidebar->setMaxWidth(200);
            sidebar->setViewportPaddingH(0);
            sidebar->setViewportPaddingV(0);
            sidebar->setReserveScrollbarGutter(true);
            sidebar->setFill(colorSpecFromRole(ColorRole::Surface));
            track(sidebar.get(), MetricKind::Scroll);
            sidebar->content()->setDirection(FlexDirection::Vertical);
            sidebar->content()->setAlign(FlexAlign::Stretch);
            auto nav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
                .axis = RovingListNavAxis::Vertical,
                .mode = RovingListNavMode::FollowFocus,
                .keepItemsInTabOrder = false,
                .scrollIntoView =
                    [this](const Node *target) {
                        if (navigation)
                            navigation->requestScrollIntoView(target);
                    },
                .syncIndexFromSelection = [this] { return std::size_t(tab); },
            });
            categoryNavigation = nav.get();
            nav->setTabFocusKey("blastirc.settings.categories");
            nav->setFillWidth(true);
            nav->setGap(2);
            nav->setPadding(6);
            track(nav.get(), MetricKind::Flex);
            for (std::size_t index = 0; index < std::size(tabNames); ++index) {
                const auto id = Tab(index);
                auto *choice = button(
                    *nav, tabNames[index],
                    [this, id] {
                        chooseTab(id);
                        if (dispatcher)
                            dispatcher->setFocus(categoryNavigation->focusArea());
                    },
                    id == tab ? ButtonVariant::TabActive : ButtonVariant::Tab);
                choice->setFillWidth(true);
                choice->setContentAlign(ButtonContentAlign::Start);
                choice->label()->setFontWeight(FontWeight::Bold);
                choice->setTooltip(tabNames[index]);
                nav->registerItem(choice, [this, id] { chooseTab(id); });
                tabButtons.emplace_back(id, choice);
            }
            sidebar->content()->addChild(std::move(nav));
            body->addChild(std::move(sidebar));
            auto detail = column(8);
            detail->setFillHeight(true);
            detail->setFlexGrow(1);
            detail->setMinWidth(0);
            detail->addChild(std::move(viewport));
            body->addChild(std::move(detail));
            chrome->addChild(std::move(body));
        } else {
            chrome->addChild(std::move(viewport));
        }
        feedback = label(*scroll->content(), "");
        feedback->setColor(colorSpecFromRole(ColorRole::Error));
        feedback->setVisible(false);
        feedback->setParticipatesInLayout(false);
        auto *footer = row(*chrome, !tabs);
        if (mode == "whois") {
            footer->setVisible(false);
            footer->setParticipatesInLayout(false);
            footer->setExcludeSubtreeFromTabOrder(true);
        }
        if (tabs) {
            auto *persistence = label(*footer, "Live · autosaves every 30s");
            persistence->setFlexGrow(1);
            persistence->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
            persistence->setTooltip(
                "Changes take effect immediately. Automatic saving is attempted "
                "every 30 seconds and on exit; invalid profiles or storage "
                "errors prevent a write. Save now requests an earlier write. "
                "Close keeps changes; it does not undo them.");
            saveButton =
                button(*footer, "Save now", [this] { saveSettings(); }, ButtonVariant::Primary);
            footer->setJustify(FlexJustify::End);
            button(*footer, "Close", [this] { dismiss(); });
        } else if (mode == "first-run") {
            saveButton = button(*footer, "Configure offline", [this] {
                if (pendingSaveRequest)
                    return;
                model.servers.clear();
                model.firstRun = false;
                save({}, true);
            });
        } else if (mode == "search") {
            button(*footer, "Find", [this] { search(); }, ButtonVariant::Primary);
            button(*footer, "Cancel", [this] { dismiss(); });
        } else if (mode == "topic") {
            auto *update =
                button(*footer, "Update topic", [this] { updateTopic(); }, ButtonVariant::Primary);
            const auto buffer = model.buffers.find(topicBuffer);
            update->setEnabled(buffer != model.buffers.end() && buffer->second.kind == "channel" &&
                               buffer->second.joined);
            button(*footer, "Cancel", [this] { dismiss(); });
        } else if (mode == "nick-color") {
            button(
                *footer, "Save color",
                [this] {
                    if (!invalidFields.empty()) {
                        setFeedback("Use #RRGGBB, or leave the field empty to use the palette.");
                        return;
                    }
                    save({}, true);
                },
                ButtonVariant::Primary);
            button(*footer, "Use palette color", [this] {
                if (pendingSaveRequest)
                    return;
                model.settings.nickColors.erase(nick);
                save({}, true);
            });
        } else if (mode != "whois") {
            button(*footer, "OK", [this] { dismiss(); }, ButtonVariant::Primary);
        }
        panel->addChild(std::move(layout));
        owner.addChild(std::move(background));
        showOverlay();
    }

    void buildSettingsCategory() {
        switch (tab) {
        case Tab::Appearance:
            appearance();
            break;
        case Tab::Keys:
            keys();
            break;
        case Tab::Chat:
            chat();
            break;
        case Tab::Fx:
            effects();
            break;
        case Tab::Sounds:
            sounds();
            break;
        case Tab::Servers:
            connection(false);
            break;
        case Tab::Identities:
            identities();
            break;
        }
    }

    void rebuildPage(bool focusContent = true) {
        if (!scroll)
            return;
        stopRecording(false);
        if (mode == "settings" && categoryPages[std::size_t(tab)]) {
            auto *oldPage = categoryPages[std::size_t(tab)];
            const auto inside = [oldPage](const Node *node) {
                for (; node; node = node->parent())
                    if (node == oldPage)
                        return true;
                return false;
            };
            std::erase_if(searchTargets, [&](const auto &target) { return inside(target.node); });
            std::erase_if(invalidFields, [&](auto *field) { return inside(field); });
            std::erase_if(identityScopes, [&](const auto &scope) { return inside(scope.first); });
            std::erase_if(sharedIdentityFields,
                          [&](const auto &field) { return inside(field.control); });
            if (tab == Tab::Keys) {
                bindingFields.clear();
            }
            if (tab == Tab::Servers) {
                networkPicker = networkIdentityPicker = nullptr;
                connectionStatus = sojuStatus = nullptr;
                connectButton = sojuSubmit = nullptr;
                displayedServer.reset();
                sojuMutationButtons.clear();
            }
            auto *allPages = page;
            detach(oldPage, *allPages);
            auto replacement = column(16);
            page = replacement.get();
            categoryPages[std::size_t(tab)] = page;
            allPages->addChild(std::move(replacement));
            currentSection.clear();
            buildSettingsCategory();
            page = allPages;
            filterSettings();
            updateIdentityScope();
            if (focusContent)
                focusFirst();
            return;
        }
        searchTargets.clear();
        categoryPages.fill(nullptr);
        bindingFields.clear();
        bindingConflicts = nullptr;
        detach(page, *scroll->content());
        themeStatus = applicationStatus = motionStatus = soundStatus = cursorStatus =
            identityScope = connectionStatus = nullptr;
        connectButton = nullptr;
        sojuStatus = nullptr;
        displayedServer.reset();
        sojuMutationButtons.clear();
        sojuSubmit = nullptr;
        topicTitle = nullptr;
        networkPicker = identityPicker = nullptr;
        invalidFields.clear();
        identityScopes.clear();
        sharedIdentityFields.clear();
        networkIdentityPicker = nullptr;
        setFeedback("");
        auto content = column(mode == "settings" ? 16 : 12);
        page = content.get();
        scroll->content()->addChild(std::move(content));
        scroll->setScrollOffset(0);
        if (mode == "settings") {
            lastApplicationStatus.clear();
            const auto selectedTab = tab;
            auto *allPages = page;
            for (std::size_t category = 0; category < categoryPages.size(); ++category) {
                tab = Tab(category);
                auto body = column(16);
                page = body.get();
                categoryPages[category] = page;
                currentSection.clear();
                allPages->addChild(std::move(body));
                buildSettingsCategory();
                const bool active = tab == selectedTab;
                page->setVisible(active);
                page->setParticipatesInLayout(active);
                page->setExcludeSubtreeFromTabOrder(!active);
            }
            tab = selectedTab;
            page = allPages;
            filterSettings();
            applicationStatus = label(*page, "");
            applicationStatus->setColor(colorSpecFromRole(ColorRole::Error));
            applicationStatus->setVisible(false);
            applicationStatus->setParticipatesInLayout(false);
            label(*page, "Changes are live. Automatic saving is attempted every 30 seconds and "
                         "on exit; fix invalid profiles or reported storage errors before "
                         "saving can succeed. Save now requests an earlier write. Close keeps "
                         "your changes; it is not Undo.")
                ->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
        } else if (mode == "whois") {
            profileLoading = profileField(*page, "Loading", "blastirc.whois.loading");
            profileError = profileField(*page, "Error", "blastirc.whois.error");
            profileError->setTextColors(themeForegroundColor("error"), {}, {}, {});
            updateProfile();
        } else if (mode == "first-run") {
            label(*page, "Make yourself at home", true);
            label(*page, "Connect directly or through your soju/ZNC bouncer. Your profiles, drafts "
                         "and workspace stay on this device.");
            connection(true);
            applicationStatus = label(*page, "");
            applicationStatus->setColor(colorSpecFromRole(ColorRole::Error));
            applicationStatus->setVisible(false);
            applicationStatus->setParticipatesInLayout(false);
            lastApplicationStatus.clear();
        } else if (mode == "search") {
            auto *query = text(*page, "Search", searchQuery,
                               [this](const std::string &value) { searchQuery = value; });
            query->setOnSubmit([this](const std::string &) { search(); });
            label(*page, "Searches saved messages in this conversation. A server-side search is "
                         "also requested when the network supports it.");
        } else if (mode == "nick-color") {
            color(*page, nick);
            label(*page, "The override previews immediately. An empty field uses the selected nick "
                         "palette.");
        } else if (mode == "config-error") {
            label(*page, configMessage.empty()
                             ? "No configuration error has been reported by the application."
                             : configMessage);
            if (!configMessage.empty())
                label(
                    *page,
                    "The application has blocked saving and automatic connections; the original "
                    "files are unchanged. Fix the reported configuration file and restart BlastIRC "
                    "to load it. Changing fields here does not repair or reload that file.");
            if (!recoveryMessage.empty())
                button(*page, "View interrupted-send recovery", [this] { open("recovery"); });
        } else if (mode == "recovery") {
            label(*page, recoveryMessage);
            label(*page, "The core restored unconfirmed text to its conversation draft. Delivery "
                         "is uncertain: inspect the conversation before sending it again. BlastIRC "
                         "does not automatically retry uncertain submissions.");
        } else if (mode == "topic") {
            const auto found = model.buffers.find(topicBuffer);
            if (found == model.buffers.end()) {
                label(*page, "This conversation no longer exists.");
            } else {
                const auto &buffer = found->second;
                topicTitle = label(*page, buffer.target + " · " + buffer.server, true);
                label(*page, std::to_string(buffer.nicks.size()) + " members · " +
                                 (buffer.joined ? "Joined" : "Not joined"));
                auto *field = text(*page, "Topic", editedTopic,
                                   [this](const std::string &value) { editedTopic = value; });
                const bool editable = buffer.kind == "channel" && buffer.joined;
                field->setEnabled(editable);
                field->setOnSubmit([this](const std::string &) { updateTopic(); });
                label(*page, editable ? "The server checks your permission to change the topic. An "
                                        "empty topic clears it."
                                      : "Join a channel to change its topic.");
            }
        }
        updateIdentityScope();
        lastThemeSource.clear();
        if (focusContent)
            focusFirst();
    }

    void appearance() {
        auto *theme = section("Theme and scale");
        static constexpr const char *sourceIds[] = {"auto", "builtin", "noctalia"};
        static constexpr const char *sourceNames[] = {"Automatic", "Built-in",
                                                      "Noctalia (built-in fallback)"};
        const std::size_t sourceIndex =
            model.settings.themeSource == "noctalia" ? 2
            : model.settings.themeSource == "builtin" || model.settings.themeSource == "built-in"
                ? 1
                : 0;
        select(*theme, "Theme source", {std::begin(sourceNames), std::end(sourceNames)},
               sourceNames[sourceIndex], [this](std::size_t index, std::string_view) {
                   if (index < std::size(sourceIds))
                       model.settings.themeSource = sourceIds[index];
               });
        themeStatus = label(*theme, std::string("Active palette: ") + themeSource());
        label(*theme, "Automatic and Noctalia both use the local Noctalia palette when present, "
                      "otherwise Built-in. An invalid palette retains the current colors.");
        toggle(*theme, "Enable transparency", model.settings.transparency,
               [this](bool value) { model.settings.transparency = value; });
        slider(*theme, "Surface opacity", model.settings.opacity, .2, 1, .01,
               [this](float value) { model.settings.opacity = value; });
        label(*theme, "Transparency is off by default. When enabled, surface opacity affects "
                      "all backgrounds, never text. Desktop transparency requires a compositor.");
        slider(*theme, "Interface scale", model.settings.scale, .7, 2, .01,
               [this](float value) { model.settings.scale = value; });
        slider(*theme, "Message text scale", model.settings.messageScale, .7, 2, .01,
               [this](float value) { model.settings.messageScale = value; });
        if (!fontsLoaded) {
            fonts = installedFonts();
            fontsLoaded = true;
        }
        auto *typography = section("Typography");
        fontPicker(*typography, "UI font family", "Installed UI fonts", model.settings.uiFont);
        fontPicker(*typography, "Message font family", "Installed message fonts",
                   model.settings.messageFont);
        if (fonts.empty())
            label(*typography,
                  "Fontconfig reported no installed families. You can still type a family name.");
        label(*typography,
              "Fontconfig resolves installed family names. Empty fields use "
              "sans-serif for interface and messages; saved overrides stay independent.");
        auto *spelling = section("Composer spelling");
        auto *spellingStatus = label(*spelling, "");
        auto languages = Spellcheck::languages();
        const auto updateSpellingStatus = [this, spellingStatus, languages] {
            const auto &language = model.settings.spellcheckLanguage;
            const bool unavailable =
                language != "auto" &&
                std::find(languages.begin(), languages.end(), language) == languages.end();
            spellingStatus->setText(
                !model.settings.spellcheck ? "Spelling is disabled."
                : unavailable
                    ? "Dictionary " + language + " is unavailable. Choose an installed dictionary."
                : language == "auto" ? "Auto uses LANG (en_US for C/POSIX). A missing dictionary "
                                       "is reported in the composer."
                                     : "Uses the local " + language +
                                           " dictionary. No text is sent over the network.");
            spellingStatus->setColor(colorSpecFromRole(model.settings.spellcheck && unavailable
                                                           ? ColorRole::Error
                                                           : ColorRole::OnSurfaceVariant));
        };
        toggle(*spelling, "Check spelling", model.settings.spellcheck,
               [this, updateSpellingStatus](bool value) {
                   model.settings.spellcheck = value;
                   updateSpellingStatus();
               });
        std::vector<std::string> languageOptions{"Auto"};
        languageOptions.insert(languageOptions.end(), languages.begin(), languages.end());
        const auto &savedLanguage = model.settings.spellcheckLanguage;
        auto *languagePicker = select(
            *spelling, "Spelling language", std::move(languageOptions),
            savedLanguage == "auto" ? std::string_view("Auto") : std::string_view(savedLanguage),
            [this, updateSpellingStatus](std::size_t index, std::string_view value) {
                model.settings.spellcheckLanguage = index == 0 ? "auto" : std::string(value);
                updateSpellingStatus();
            });
        if (savedLanguage != "auto" &&
            std::find(languages.begin(), languages.end(), savedLanguage) == languages.end())
            languagePicker->setPlaceholder(savedLanguage + " (unavailable)");
        updateSpellingStatus();
        auto *cursor = section("Composer cursor");
        static constexpr const char *shapeIds[] = {"line", "box", "underline"};
        static constexpr const char *shapeNames[] = {"Line", "Solid box", "Underline"};
        const std::size_t shapeIndex = model.settings.cursorShape == "box"         ? 1
                                       : model.settings.cursorShape == "underline" ? 2
                                                                                   : 0;
        select(*cursor, "Cursor shape", {std::begin(shapeNames), std::end(shapeNames)},
               shapeNames[shapeIndex], [this](std::size_t index, std::string_view) {
                   if (index < std::size(shapeIds))
                       model.settings.cursorShape = shapeIds[index];
               });
        static constexpr const char *effectIds[] = {"off", "trail", "comet", "ribbon", "pulse"};
        static constexpr const char *effectNames[] = {"Off", "Trail", "Comet", "Ribbon", "Pulse"};
        std::size_t effectIndex = 0;
        for (std::size_t i = 0; i < std::size(effectIds); ++i)
            if (model.settings.cursorEffect == effectIds[i])
                effectIndex = i;
        select(*cursor, "Cursor animation", {std::begin(effectNames), std::end(effectNames)},
               effectNames[effectIndex], [this](std::size_t index, std::string_view) {
                   if (index < std::size(effectIds))
                       model.settings.cursorEffect = effectIds[index];
               });
        cursorStatus = label(*cursor, "");
        slider(*cursor, "Animation duration (40–240 ms)", model.settings.cursorTrailDurationMs, 40,
               240, 1, [this](float value) { model.settings.cursorTrailDurationMs = value; });
        slider(*cursor, "Animation strength (0.05–0.35)", model.settings.cursorTrailStrength, .05,
               .35, .01, [this](float value) { model.settings.cursorTrailStrength = value; });
        label(*cursor, "Trail leaves a fading path; Comet tapers its tail; Ribbon bends between "
                       "positions; Pulse expands at the destination. Duration and strength tune "
                       "the selected animation. Preview in the composer; Effects Off and Reduced "
                       "motion suppress animation without changing your preferences.");
        button(*cursor, "Try in conversation", [this] { dismiss(); });
        auto *messages = section("Message display");
        label(*messages, "Channels and direct messages use grouped bubbles by default. "
                         "Choose traditional IRC rows from each conversation's menu.");
        toggle(*messages, "Show timestamps", model.settings.timestamps,
               [this](bool value) { model.settings.timestamps = value; });
        toggle(*messages, "Show avatars", model.settings.avatars,
               [this](bool value) { model.settings.avatars = value; });
        select(*messages, "Avatar style", {"sprite", "monogram", "initials", "none"},
               model.settings.avatarStyle,
               [this](std::size_t, std::string_view value) { model.settings.avatarStyle = value; });
        select(*messages, "Nick palette", {"THEME", "CLASSIC", "VIVID"}, model.settings.nickPalette,
               [this](std::size_t, std::string_view value) { model.settings.nickPalette = value; });
        slider(*messages, "Message wrap columns (0 = unlimited)", model.settings.messageWrapColumns,
               0, 1000, 1, [this](float value) { model.settings.messageWrapColumns = int(value); });
        label(*messages, "Wrap at whole characters and word boundaries, or earlier to fit the "
                         "window. This never adds newlines to copied text.");
        static constexpr const char *foolStyles[] = {"Minimized", "Blurred", "Dimmed", "Hidden"};
        select(*messages, "Fool messages", {std::begin(foolStyles), std::end(foolStyles)},
               foolStyles[std::clamp(int(model.settings.foolDisplay), 0, 3)],
               [this](std::size_t index, std::string_view) {
                   if (index < std::size(foolStyles))
                       model.settings.foolDisplay = FoolDisplay(index);
               });
        toggle(*messages, "Count fool messages as unread", model.settings.foolUnread,
               [this](bool value) { model.settings.foolUnread = value; });
        label(*messages, "Fools never trigger notifications, sounds, typing or incoming effects. "
                         "Minimized groups and blurred bodies require reveal before copying; "
                         "dimmed text remains readable. Hidden messages stay in local history. "
                         "Unread counts are off by default so muted activity stays quiet.");
        auto *previews = section("Link and media previews");
        static constexpr const char *previewModes[] = {"Off", "Click to load", "Automatic"};
        select(*previews, "Link previews", {std::begin(previewModes), std::end(previewModes)},
               previewModes[std::clamp(int(model.settings.linkPreviews), 0, 2)],
               [this](std::size_t index, std::string_view) {
                   if (index < std::size(previewModes))
                       model.settings.linkPreviews = PreviewMode(index);
               });
        select(*previews, "Image previews", {std::begin(previewModes), std::end(previewModes)},
               previewModes[std::clamp(int(model.settings.imagePreviews), 0, 2)],
               [this](std::size_t index, std::string_view) {
                   if (index < std::size(previewModes))
                       model.settings.imagePreviews = PreviewMode(index);
               });
        select(*previews, "Audio and video previews",
               {std::begin(previewModes), std::end(previewModes)},
               previewModes[std::clamp(int(model.settings.mediaPreviews), 0, 2)],
               [this](std::size_t index, std::string_view) {
                   if (index < std::size(previewModes))
                       model.settings.mediaPreviews = PreviewMode(index);
               });
        integer(*previews, "Preview thumbnail height (64–240 px)", model.settings.previewMaxHeight,
                64, 240, [this](int value) { model.settings.previewMaxHeight = value; });
        integer(*previews, "Media size limit (1–512 MiB)", model.settings.previewMediaMaxMiB, 1,
                512, [this](int value) { model.settings.previewMediaMaxMiB = value; });
        integer(*previews, "Media duration limit (1–7200 seconds)",
                model.settings.previewMediaMaxSeconds, 1, 7200,
                [this](int value) { model.settings.previewMediaMaxSeconds = value; });
        label(*previews,
              "Click to load is the privacy default: no preview request is sent until "
              "you choose to load it. Automatic requests can disclose your IP to remote "
              "hosts. Link, image and media permissions are independent; media never "
              "autoplays. Local and private network destinations are blocked by default.");
        toggle(*previews, "Allow local-network preview requests", model.settings.previewAllowLocal,
               [this](bool value) { model.settings.previewAllowLocal = value; });
        label(*previews, "Advanced: enabling local requests allows previews to contact loopback "
                         "and private-network services, including metadata endpoints. This can "
                         "expose local services and private data to links posted by other people. "
                         "Automatic preview modes also use this permission. Leave off unless you "
                         "trust the links and your network.");
        auto *workspace = section("Conversation navigation");
        label(*workspace, "The channel sidebar expands to names or compacts to avatars. "
                          "Channel hints navigate the selected server; the buffer picker "
                          "reaches conversations across all servers.");
        static constexpr const char *peopleModes[] = {"Expanded", "Avatars only", "Hidden"};
        select(*workspace, "People for the focused channel",
               {std::begin(peopleModes), std::end(peopleModes)},
               peopleModes[std::clamp(int(model.settings.peopleMode), 0, 2)],
               [this](std::size_t index, std::string_view) {
                   model.settings.peopleMode = SidebarMode(index);
               });
        label(*workspace,
              "The selected People mode is kept when temporarily hidden outside channels or "
              "when the conversation needs more room.");
        label(*workspace, commandLabel(model, "Channel hints", "jump-channel") + " · " +
                              commandLabel(model, "All conversations", "switch-buffer"));
    }

    void fontPicker(Flex &parent, std::string_view familyLabel, std::string_view installedLabel,
                    std::string &family) {
        auto *field = text(parent, familyLabel, family, {});
        field->setSubmitOnFocusLoss(false);
        auto options = std::make_shared<std::vector<std::string>>(fonts);
        auto *picker = select(parent, installedLabel, *options, family,
                              [field, &family](std::size_t, std::string_view value) {
                                  family = value;
                                  field->setValue(value);
                              });
        picker->setNotifyOnReselect(true);
        const auto syncSelection = [picker, options, &family] {
            const auto found = std::find(options->begin(), options->end(), family);
            picker->clearSelection();
            if (found != options->end())
                picker->setSelectedIndexSilently(std::size_t(found - options->begin()));
            else
                picker->setPlaceholder(family.empty() ? "Style default" : family);
        };
        field->setOnSubmit([&family, syncSelection](const std::string &value) {
            family = value;
            syncSelection();
        });
        label(parent, "Type to find installed fonts. Choose a family above, or press Enter "
                      "in the family field to apply an arbitrary family name. "
                      "Closing discards an unapplied query.");
        field->setOnChange([this, picker, options, syncSelection](const std::string &query) {
            options->clear();
            for (const auto &font : fonts)
                if (ui_logic::fuzzy(query, font) > -100000)
                    options->push_back(font);
            std::stable_sort(options->begin(), options->end(),
                             [&query](const auto &a, const auto &b) {
                                 return ui_logic::fuzzy(query, a) > ui_logic::fuzzy(query, b);
                             });
            picker->setOptions(*options);
            syncSelection();
        });
    }

    void keys() {
        static constexpr const char *groups[] = {"Messaging", "Cursor movement",
                                                 "Editing and selection", "Navigation",
                                                 "Other commands"};
        const auto groupFor = [](std::string_view name) {
            if (name == "send" || name == "newline" || name == "complete" ||
                name.starts_with("history-"))
                return 0;
            if (name.starts_with("beginning-") || name.starts_with("end-") ||
                name == "backward-char" || name == "forward-char" || name == "previous-line" ||
                name == "next-line" || name == "backward-word" || name == "forward-word")
                return 1;
            if (name.find("kill") != std::string_view::npos || name == "delete-char" ||
                name == "backward-delete" || name == "copy-region" || name == "yank" ||
                name == "undo" || name == "set-mark" || name == "redo" || name == "copy" ||
                name == "cut" || name == "paste" || name == "select-all" ||
                name == "transpose-chars")
                return 2;
            if (name == "switch-buffer" || name == "execute-command" || name == "jump-channel" ||
                name.starts_with("toggle-") || name == "search" || name == "settings" ||
                name == "cancel")
                return 3;
            return 4;
        };
        for (int category = 0; category < int(std::size(groups)); ++category) {
            Flex *tasks = nullptr;
            for (const auto &[name, chord] : model.settings.bindings) {
                if (groupFor(name) != category)
                    continue;
                if (!tasks) {
                    tasks = section(groups[category]);
                    label(*tasks, "Action / Current binding · Default · Record / Reset", true);
                }
                auto group = std::make_unique<BindingRow>();
                group->setDirection(FlexDirection::Vertical);
                group->setAlign(FlexAlign::Stretch);
                group->setFillWidth(true);
                group->setGap(8);
                track(group.get(), MetricKind::Flex);
                auto *field = text(*group, commandDisplayName(name), chord,
                                   [this, name](const std::string &value) {
                                       model.settings.bindings[name] = value;
                                   });
                auto *fieldGroup = dynamic_cast<Flex *>(field->parent());
                fieldGroup->setFillWidth(false);
                fieldGroup->setMinWidth(200);
                fieldGroup->setMaxWidth(320);
                for (auto &metric : metrics)
                    if (metric.node == fieldGroup) {
                        metric.minWidth = 200;
                        metric.maxWidth = 320;
                    }
                fieldGroup->setFlexGrow(1);
                bindingFields[name] = field;
                searchTargets.back().terms += " " + name;
                field->inputArea()->setTabFocusKey("blastirc.settings.binding." + name);
                field->inputArea()->setTooltip(
                    "Current binding for " + name +
                    ". Space separates strokes; C = Control, M = Alt, S = Shift. "
                    "RET, TAB, SPC and BACKSPACE name special keys.");
                auto *actions = row(*group, false);
                actions->setFillWidth(false);
                actions->setMinWidth(230);
                for (auto &metric : metrics)
                    if (metric.node == actions)
                        metric.minWidth = 230;
                actions->setFlexGrow(1);
                const auto defaults = defaultBindings().find(name);
                const std::string defaultChord =
                    defaults == defaultBindings().end() ? "" : defaults->second;
                auto *secondary =
                    label(*actions, "Default: " + (defaultChord.empty() ? std::string("Unbound")
                                                                        : defaultChord));
                secondary->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
                secondary->setFlexGrow(1);
                auto *record = button(*actions, "Record", {});
                record->inputArea()->setTabFocusKey("blastirc.settings.record." + name);
                record->setTooltip("Record key sequence, including Tab and Escape. Click Finish "
                                   "or Ctrl+Enter to apply; Ctrl+G cancels.");
                record->inputArea()->setRetainsFocusOnPointerRelease(true);
                record->setOnClick([this, record, field, name] {
                    if (recordingButton == record)
                        stopRecording(true);
                    else
                        startRecording(record, field, name);
                });
                record->inputArea()->setOnKeyDown([this, record, field, name](const auto &key) {
                    if (recordingButton == record)
                        recordKey(key);
                    else if (key.pressed && KeybindMatcher::matches(KeybindAction::Validate,
                                                                    key.sym, key.modifiers))
                        startRecording(record, field, name);
                });
                if (defaults != defaultBindings().end()) {
                    auto *reset = button(*actions, "Reset", [this, name, field] {
                        stopRecording(false);
                        model.settings.bindings[name] = defaultBindings().at(name);
                        field->setValue(model.settings.bindings.at(name));
                    });
                    reset->inputArea()->setTabFocusKey("blastirc.settings.reset." + name);
                }
                tasks->addChild(std::move(group));
            }
            if (category == 0 && tasks)
                label(*tasks,
                      "Bindings are live. Record captures Tab and Escape; Finish or Ctrl+Enter "
                      "applies the sequence, Ctrl+G cancels and returns to normal focus. "
                      "Shift+Tab moves between controls outside recording.");
        }
        button(*page, "Restore default keymap", [this] {
            stopRecording(false);
            model.settings.bindings = defaultBindings();
            for (const auto &[name, field] : bindingFields)
                field->setValue(model.settings.bindings.at(name));
        });
        bindingConflicts = label(*page, "");
        bindingConflicts->setColor(colorSpecFromRole(ColorRole::Error));
        lastConflictBindings.clear();
    }

    void effects() {
        auto *motion = section("Motion and sound policy");
        const char *levels[] = {"Off", "Subtle", "Full"};
        select(*motion, "Effects", {"Off", "Subtle", "Full"},
               levels[std::clamp(int(model.settings.fx), 0, 2)],
               [this](std::size_t index, std::string_view) { model.settings.fx = FxLevel(index); });
        toggle(*motion, "Reduced motion", model.settings.reducedMotion,
               [this](bool value) { model.settings.reducedMotion = value; });
        motionStatus = label(*motion, "");
        label(*motion, "Effects Off suppresses event motion, cursor animations and event sounds. "
                       "Reduced motion suppresses animation, not enabled sound. "
                       "Suppressed preferences below are kept, not reset.");
        auto *sending = section("Sending messages");
        static constexpr const char *movements[] = {"None", "Flight", "Rise",
                                                    "Pop",  "Slide",  "Drift"};
        select(*sending, "Sending movement", {std::begin(movements), std::end(movements)},
               movements[std::clamp(int(model.settings.sendMotion), 0, 5)],
               [this](std::size_t index, std::string_view) {
                   if (index < std::size(movements))
                       model.settings.sendMotion = SendMotion(index);
               });
        static constexpr const char *reveals[] = {"None", "Fade", "Pixels"};
        select(*sending, "Sending reveal", {std::begin(reveals), std::end(reveals)},
               reveals[std::clamp(int(model.settings.sendReveal), 0, 2)],
               [this](std::size_t index, std::string_view) {
                   if (index < std::size(reveals))
                       model.settings.sendReveal = SendReveal(index);
               });
        label(*sending, "Movement and reveal are independent for sent bubbles. Flight travels "
                        "from the composer, Rise slides upward, and Pop grows into place. "
                        "Slide enters from the side; Drift eases gently into place. "
                        "Fade changes opacity; Pixels uncovers the message in blocks. "
                        "Choose None in either selector to keep only the other effect.");
        auto *incoming = section("Incoming messages");
        static constexpr SendMotion incomingValues[] = {SendMotion::None, SendMotion::Rise,
                                                        SendMotion::Pop, SendMotion::Slide,
                                                        SendMotion::Drift};
        static constexpr const char *incomingNames[] = {
            "None", "Rise · upward", "Pop · grow into place", "Slide · from the side",
            "Drift · gentle settling"};
        std::size_t incomingIndex = 0;
        for (std::size_t i = 0; i < std::size(incomingValues); ++i)
            if (model.settings.incomingMotion == incomingValues[i])
                incomingIndex = i;
        select(*incoming, "Incoming movement", {std::begin(incomingNames), std::end(incomingNames)},
               incomingNames[incomingIndex], [this](std::size_t index, std::string_view) {
                   if (index < std::size(incomingValues))
                       model.settings.incomingMotion = incomingValues[index];
               });
        static constexpr const char *incomingReveals[] = {"None", "Fade · opacity",
                                                          "Pixels · uncover in blocks"};
        select(*incoming, "Incoming reveal",
               {std::begin(incomingReveals), std::end(incomingReveals)},
               incomingReveals[std::clamp(int(model.settings.incomingReveal), 0, 2)],
               [this](std::size_t index, std::string_view) {
                   if (index < std::size(incomingReveals))
                       model.settings.incomingReveal = SendReveal(index);
               });
        slider(*incoming, "Incoming duration (100–2000 ms)", model.settings.incomingDurationMs, 100,
               2000, 10, [this](float value) { model.settings.incomingDurationMs = value; });
        label(*incoming, "Movement and reveal are independent from sending effects. None plus "
                         "Fade is the default. Only new live messages animate; restored history "
                         "and fool activity stay still. Effects Off and Reduced motion suppress "
                         "animation without resetting these preferences.");
        auto *physics = section("Advanced · Event physics");
        auto *gravity = slider(*physics, "Gravity", model.settings.gravity, -200, 500, 1,
                               [this](float value) { model.settings.gravity = value; });
        auto *drag = slider(*physics, "Particle drag", model.settings.drag, 0, 10, .01,
                            [this](float value) { model.settings.drag = value; });
        auto *shake = slider(*physics, "Overlay shake", model.settings.shake, 0, 3, .01,
                             [this](float value) { model.settings.shake = value; });
        button(*physics, "Reset physics", [this, gravity, drag, shake] {
            model.settings.gravity = 80;
            model.settings.drag = model.settings.shake = 1;
            gravity->setValue(80);
            drag->setValue(1);
            shake->setValue(1);
        });
    }

    void sounds() {
        auto *master = section("Playback");
        toggle(*master, "Sound enabled", model.settings.sound,
               [this](bool value) { model.settings.sound = value; });
        soundStatus = label(*master, "");
        slider(*master, "Master volume", model.settings.masterVolume, 0, 1, .01,
               [this](float value) { model.settings.masterVolume = value; });
        select(*master, "Pitch variation", {"natural", "musical", "fixed"},
               model.settings.pitchVariation, [this](std::size_t, std::string_view value) {
                   model.settings.pitchVariation = value;
               });
        label(*master, "Effects Off also mutes event sounds. Reduced motion does not mute them.");
        auto *cues = section("Event cues");
        for (const auto &[name, cue] : model.settings.cues) {
            auto group = column(12);
            toggle(*group, name, cue.enabled,
                   [this, name](bool value) { model.settings.cues.at(name).enabled = value; });
            slider(*group, "Volume", cue.volume, 0, 1, .01,
                   [this, name](float value) { model.settings.cues.at(name).volume = value; });
            slider(*group, "Pitch", cue.pitch, .25, 4, .01,
                   [this, name](float value) { model.settings.cues.at(name).pitch = value; });
            cues->addChild(std::move(group));
        }
    }

    void color(Flex &parent, const std::string &name) {
        auto existing = model.settings.nickColors.find(name);
        const std::string value =
            existing == model.settings.nickColors.end() ? "" : existing->second;
        auto *field = text(parent, name + " color (#RRGGBB)", value, {});
        field->setPlaceholder("Empty uses the nick palette");
        field->setOnChange([this, name, field](const std::string &next) {
            const bool valid = next.empty() || colorValid(next);
            field->setInvalid(!valid);
            if (valid) {
                invalidFields.erase(field);
                if (next.empty())
                    model.settings.nickColors.erase(name);
                else
                    model.settings.nickColors[name] = next;
            } else
                invalidFields.insert(field);
        });
        if (!value.empty() && !colorValid(value)) {
            field->setInvalid(true);
            invalidFields.insert(field);
        }
    }

    void chat() {
        auto *uploads = section("File uploads");
        std::vector<std::string> providers;
        for (int i = 0; i <= int(UploadProvider::Soju); ++i)
            providers.emplace_back(uploadProviderLabel(UploadProvider(i)));
        select(*uploads, "Default file host", providers,
               std::string(uploadProviderLabel(model.settings.uploads.provider)),
               [this](std::size_t index, std::string_view) {
                   model.settings.uploads.provider = UploadProvider(index);
                   model.settings.uploads.maxBytes =
                       std::min(model.settings.uploads.maxBytes,
                                uploadProviderLimit(model.settings.uploads.provider));
                   rebuildPage();
               });
        integer(*uploads, "Upload size limit (MiB)", int(model.settings.uploads.maxBytes / 1048576),
                1, int(uploadProviderLimit(model.settings.uploads.provider) / 1048576),
                [this](int value) { model.settings.uploads.maxBytes = uint64_t(value) * 1048576; });
        const auto provider = model.settings.uploads.provider;
        if (provider == UploadProvider::CrafterBin || provider == UploadProvider::Custom) {
            toggle(*uploads, "Secret / unlisted upload", model.settings.uploads.secret,
                   [this](bool value) { model.settings.uploads.secret = value; });
            auto *expiry = text(*uploads, "Expiry hours (empty means host default)",
                                model.settings.uploads.expiryHours, {});
            expiry->setOnChange([this, expiry](const std::string &value) {
                auto next = model.settings.uploads;
                next.expiryHours = value;
                const bool valid = validateUploadConfig(next).empty();
                expiry->setInvalid(!valid);
                if (valid) {
                    model.settings.uploads.expiryHours = value;
                    invalidFields.erase(expiry);
                } else
                    invalidFields.insert(expiry);
            });
        }
        if (provider == UploadProvider::Litterbox)
            select(*uploads, "Litterbox retention", {"1h", "12h", "24h", "72h"},
                   model.settings.uploads.litterboxExpiry,
                   [this](std::size_t, std::string_view value) {
                       model.settings.uploads.litterboxExpiry = value;
                   });
        if (provider == UploadProvider::Custom) {
            auto *endpoint =
                text(*uploads, "Custom HTTPS endpoint", model.settings.uploads.customEndpoint, {});
            auto *username =
                text(*uploads, "Custom HTTP username", model.settings.uploads.username,
                     [this](const std::string &value) { model.settings.uploads.username = value; });
            auto *password = text(
                *uploads, "Custom upload password", model.settings.uploads.password,
                [this](const std::string &value) { model.settings.uploads.password = value; },
                true);
            endpoint->setOnChange([this, endpoint, username, password](const std::string &value) {
                auto &config = model.settings.uploads;
                auto next = config;
                next.customEndpoint = value;
                const bool valid = validateUploadConfig(next).empty();
                endpoint->setInvalid(!valid);
                if (valid) {
                    if (!sameUploadAuthority(config.customEndpoint, value)) {
                        config.username.clear();
                        config.password.clear();
                        username->setValue("");
                        password->setValue("");
                    }
                    config.customEndpoint = value;
                    invalidFields.erase(endpoint);
                } else
                    invalidFields.insert(endpoint);
            });
            label(*uploads, "Changing the HTTPS authority clears saved credentials. Passwords "
                            "are stored in plain text in your private configuration file.");
        }
        label(*uploads,
              provider == UploadProvider::Soju
                  ? "Uses this conversation's authenticated soju account FILEHOST. Configure any "
                    "extra "
                    "trusted hostname on that account. No public-host fallback."
                  : "Uploads leave this device. Links may be public; secret links are not access "
                    "control. Host retention is independent of local history. Review before "
                    "uploading; "
                    "completed links are never sent automatically.");
        auto *presence = section("Presence and typing");
        const char *modes[] = {"All", "Smart", "Hidden"};
        select(*presence, "Presence rows", {"All", "Smart", "Hidden"},
               modes[std::clamp(int(model.settings.presence), 0, 2)],
               [this](std::size_t index, std::string_view) {
                   model.settings.presence = PresenceMode(index);
               });
        label(*presence,
              "Smart shows recent conversational participants. Channel menus can override "
              "the global setting.");
        toggle(*presence, "Send typing indicators", model.settings.typingSend,
               [this](bool value) { model.settings.typingSend = value; });
        toggle(*presence, "Show typing indicators", model.settings.typingReceive,
               [this](bool value) { model.settings.typingReceive = value; });
        auto *people = section("People");
        text(*people, "Friends", joined(model.settings.friends), [this](const std::string &value) {
            const auto parsed = words(value);
            model.settings.friends = {parsed.begin(), parsed.end()};
        });
        text(*people, "Fools", joined(model.settings.fools), [this](const std::string &value) {
            const auto parsed = words(value);
            model.settings.fools = {parsed.begin(), parsed.end()};
        });
        auto *colors = section("Nick color overrides");
        for (const auto &[name, value] : model.settings.nickColors) {
            (void)value;
            auto group = column(4);
            color(*group, name);
            button(*group, "Remove " + name, [this, name] {
                model.settings.nickColors.erase(name);
                rebuildPage();
            });
            colors->addChild(std::move(group));
        }
        label(*colors, "Add overrides from a member's context menu.");
        auto *history = section("Local history");
        integer(*history, "Retention days (1–3650)", model.settings.retentionDays, 1, 3650,
                [this](int value) { model.settings.retentionDays = value; });
        integer(*history, "Messages in memory / saved history (200–100000)",
                model.settings.maxMessages, 200, 100000,
                [this](int value) { model.settings.maxMessages = value; });
        label(*history, "History is bounded in memory and SQLite. Clearing local history does not "
                        "delete anything from your server.");
        button(
            *history, "Clear saved history…", [this] { clearHistory(); },
            ButtonVariant::Destructive);
    }

    std::string_view networkCaption(const ServerConfig &profile) const {
        if (!profile.alias.empty())
            return profile.alias;
        if (!profile.sojuParent.empty()) {
            const auto parent = model.networks.find(profile.sojuParent);
            if (parent != model.networks.end()) {
                const auto remote = parent->second.sojuNetworks.find(profile.sojuNetworkId);
                if (remote != parent->second.sojuNetworks.end()) {
                    const auto name = remote->second.attributes.find("name");
                    if (name != remote->second.attributes.end() && !name->second.empty())
                        return name->second;
                }
            }
        }
        return profile.name;
    }

    std::string_view networkParentCaption(const ServerConfig &profile) const {
        if (profile.sojuParent.empty())
            return {};
        const auto parent =
            std::find_if(model.servers.begin(), model.servers.end(),
                         [&](const auto &entry) { return entry.name == profile.sojuParent; });
        return parent == model.servers.end() ? std::string_view(profile.sojuParent)
                                             : networkCaption(*parent);
    }

    std::vector<std::string> networkLabels() {
        networkOptions.clear();
        std::vector<std::string> result;
        for (const auto &profile : model.servers) {
            networkOptions.push_back({profile.name, std::string(networkCaption(profile)),
                                      std::string(networkParentCaption(profile))});
            result.push_back(networkOptions.back().caption);
        }
        for (std::size_t i = 0; i < networkOptions.size(); ++i) {
            const auto &entry = networkOptions[i];
            const bool duplicate =
                std::count_if(networkOptions.begin(), networkOptions.end(), [&](const auto &other) {
                    return other.caption == entry.caption;
                }) > 1;
            if (duplicate && !entry.parentCaption.empty())
                result[i] += " · " + entry.parentCaption;
        }
        std::vector<bool> ambiguous(result.size());
        for (std::size_t i = 0; i < result.size(); ++i)
            ambiguous[i] = std::count(result.begin(), result.end(), result[i]) > 1;
        for (std::size_t i = 0; i < result.size(); ++i)
            if (ambiguous[i])
                result[i] += " [" + networkOptions[i].key + "]";
        return result;
    }

    std::vector<std::string> identityNames() const {
        std::vector<std::string> result;
        for (const auto &identity : model.identities)
            result.push_back(identity.name);
        return result;
    }

    void addIdentity() {
        Identity identity;
        for (std::size_t n = model.identities.size() + 1;; ++n) {
            identity.name = "identity" + std::to_string(n);
            if (std::none_of(model.identities.begin(), model.identities.end(),
                             [&](const auto &existing) { return existing.name == identity.name; }))
                break;
        }
        model.identities.push_back(std::move(identity));
        identityIndex = model.identities.size() - 1;
        if (networkIdentityPicker && serverIndex < model.servers.size()) {
            const auto names = identityNames();
            const auto found =
                std::find(names.begin(), names.end(), model.servers[serverIndex].identity);
            networkIdentityPicker->setOptions(names);
            if (found != names.end())
                networkIdentityPicker->setSelectedIndexSilently(std::size_t(found - names.begin()));
        }
    }

    void ensureIdentity() {
        if (model.identities.empty())
            model.identities.emplace_back();
    }

    void addServer() {
        ensureIdentity();
        ServerConfig server;
        server.identity = model.identities.front().name;
        for (std::size_t n = model.servers.size() + 1;; ++n) {
            server.name = "Network " + std::to_string(n);
            if (std::none_of(model.servers.begin(), model.servers.end(),
                             [&](const auto &existing) { return existing.name == server.name; }) &&
                std::none_of(model.buffers.begin(), model.buffers.end(),
                             [&](const auto &entry) { return entry.second.server == server.name; }))
                break;
        }
        model.servers.push_back(std::move(server));
        serverIndex = model.servers.size() - 1;
    }

    void connect(std::size_t index) {
        if (pendingSaveRequest)
            return;
        if (!invalidFields.empty()) {
            setFeedback("Correct the highlighted fields before connecting.");
            return;
        }
        if (auto error = validateServerProfile(model, index); !error.empty()) {
            setFeedback(error);
            return;
        }
        const std::string name = model.servers[index].name;
        model.firstRun = false;
        save(name);
    }

    std::string sharingNetworks(std::string_view identity) const {
        std::string result;
        for (const auto &server : model.servers) {
            if (server.identity != identity)
                continue;
            if (!result.empty())
                result += ", ";
            result += std::string(networkCaption(server));
        }
        return result.empty() ? "No networks use this identity." : "Used by: " + result + ".";
    }

    void updateIdentityScope() {
        for (const auto &[control, index] : identityScopes) {
            if (index >= model.identities.size())
                continue;
            const auto &identity = model.identities[index];
            control->setText("Shared identity: " + identity.name + " · " + identity.nick + " (" +
                             identity.username + "). " + sharingNetworks(identity.name));
        }
    }

    std::string connectionState(const ServerConfig &server) const {
        const auto runtime = model.networks.find(server.name);
        std::string state;
        if (!server.sojuEnabled && !server.sojuParent.empty())
            state = "Disabled locally";
        else if (runtime != model.networks.end())
            state = runtime->second.state;
        else if (!server.sojuParent.empty()) {
            const auto parent = model.networks.find(server.sojuParent);
            state = parent != model.networks.end() && parent->second.state == "Ready"
                        ? "Waiting for attachment"
                        : "Waiting for account connection";
        } else
            state = server.sojuEnabled ? "Not connected" : "Startup connection disabled";
        if (runtime != model.networks.end() && !runtime->second.error.empty())
            state += " — " + runtime->second.error;
        return std::string(networkCaption(server)) + ": " + state;
    }

    void updateConnectionStatus(const ServerConfig &server) {
        if (!connectionStatus)
            return;
        std::string text;
        if (!server.sojuParent.empty()) {
            const auto parent =
                std::find_if(model.servers.begin(), model.servers.end(),
                             [&](const auto &entry) { return entry.name == server.sojuParent; });
            if (parent != model.servers.end())
                text = connectionState(*parent) + "\n";
        }
        text += connectionState(server);
        if (server.sojuAccount)
            for (const auto &child : model.servers)
                if (child.sojuParent == server.name)
                    text += "\n" + connectionState(child);
        connectionStatus->setText(text);
    }

    Input *sharedIdentityText(Flex &parent, std::string_view name, std::size_t index,
                              std::string Identity::*member, bool password = false) {
        auto *field = text(parent, name, model.identities.at(index).*member, {}, password);
        sharedIdentityFields.push_back({field, index, member});
        field->setOnChange([this, index, member](const std::string &value) {
            model.identities.at(index).*member = value;
            for (const auto &other : sharedIdentityFields)
                if (other.index == index && other.member == member &&
                    other.control->value() != value)
                    other.control->setValue(value);
            updateIdentityScope();
        });
        return field;
    }
    void identityFields(Flex &parent, std::size_t index) {
        const auto &identity = model.identities.at(index);
        auto *nickname = text(parent, "Nickname", identity.nick, {});
        nickname->setInvalid(!irc::validNickname(identity.nick));
        nickname->setOnChange([this, index, nickname](const std::string &value) {
            model.identities.at(index).nick = value;
            nickname->setInvalid(!irc::validNickname(value));
            updateIdentityScope();
        });
        sharedIdentityText(parent, "Username", index, &Identity::username);
        text(parent, "Real name", identity.realname, [this, index](const std::string &value) {
            model.identities.at(index).realname = value;
        });
        label(parent, "Shared identity credentials", true);
        sharedIdentityText(parent, "SASL password (PLAIN)", index, &Identity::password, true);
        label(parent, "Identity passwords are stored in plain text in your private configuration "
                      "file. PLAIN requires TLS; select the authentication method per network.");
        text(parent, "Client certificate (PEM, EXTERNAL)", identity.certificate,
             [this, index](const std::string &value) {
                 model.identities.at(index).certificate = value;
             });
        text(parent, "Private key (PEM, EXTERNAL)", identity.key,
             [this, index](const std::string &value) { model.identities.at(index).key = value; });
    }

    void sojuAssociation(const std::string &legacy, Flex &parent) {
        for (const auto &account : model.servers) {
            if (!account.sojuAccount)
                continue;
            const auto runtime = model.networks.find(account.name);
            if (runtime == model.networks.end())
                continue;
            for (const auto &[id, info] : runtime->second.sojuNetworks) {
                const auto title = info.attributes.find("name");
                button(
                    parent,
                    "Associate with " + account.name + " / " +
                        (title == info.attributes.end() ? id : title->second) + " [" + id + "]",
                    [this, legacy, name = account.name, id] {
                        confirm(
                            "associate-soju", "Associate existing local profile?",
                            "Bind this exact local profile and its retained conversations to the "
                            "selected account and remote network ID? No remote network is created "
                            "or deleted. Shared identity credentials are not changed.",
                            "Associate profile", [this, legacy, name, id] {
                                model.actions.push_back(
                                    {"soju-associate-network", legacy, name, id});
                                rebuildPage();
                            });
                    });
            }
        }
        label(parent, "Ambiguous legacy profiles are never merged automatically. Connect the "
                      "intended account to discover IDs, then explicitly associate this profile.");
    }

    void deleteSojuNetwork(const std::string &parent, const std::string &id) {
        confirm("delete-soju-network", "Delete remote soju network?",
                "Delete remote network [" + id +
                    "] from this soju account? This disconnects the "
                    "upstream network for all clients, not just BlastIRC. Local history, drafts, "
                    "queued-send recovery and read state are retained in the global conversation "
                    "picker.",
                "Delete remote network", [this, parent, id] {
                    model.actions.push_back({"soju-delete-network", parent, id, {}});
                });
    }

    void sojuManagement(const std::string &parent) {
        if (sojuEditParent != parent) {
            sojuEditParent = parent;
            sojuEditId.clear();
            sojuEditMode = false;
            sojuEditAttributes.clear();
            sojuEditChanges.clear();
        }
        auto *manager = section("Remote soju networks");
        const auto found = model.networks.find(parent);
        const bool capable = found != model.networks.end() &&
                             found->second.caps.contains("soju.im/bouncer-networks") &&
                             found->second.state == "Ready";
        sojuStatus = label(*manager, "");
        label(*manager,
              "Create, edit and delete affect the actual remote soju account. New "
              "networks attach locally by default. Local enable is independent of "
              "the upstream connection; remote deletion retains local conversation data.");
        button(*manager, "Refresh displayed catalog", [this] { rebuildPage(); });
        if (found != model.networks.end()) {
            for (const auto &[id, info] : found->second.sojuNetworks) {
                const auto name = info.attributes.find("name");
                label(*manager,
                      (name == info.attributes.end() ? id : name->second) + " [" + id + "]", true);
                for (const auto &[key, value] : info.attributes)
                    if (key != "pass" && key != "sasl-password" && key != "password")
                        label(*manager, key + ": " + value);
                auto *actions = row(*manager);
                auto *edit = button(*actions, "Edit remote network", [this, id, info] {
                    sojuEditId = id;
                    sojuEditMode = true;
                    sojuEditAttributes = info.attributes;
                    sojuEditChanges.clear();
                    sojuEditAttributes.erase("pass");
                    sojuEditAttributes.erase("sasl-password");
                    rebuildPage();
                });
                edit->setEnabled(capable && !found->second.sojuMutationPending);
                sojuMutationButtons.push_back(edit);
                auto *remove = button(
                    *actions, "Delete remote network…",
                    [this, parent, id] { deleteSojuNetwork(parent, id); },
                    ButtonVariant::Destructive);
                remove->setEnabled(capable && !found->second.sojuMutationPending);
                sojuMutationButtons.push_back(remove);
                for (const auto &child : model.servers)
                    if (child.sojuParent == parent && child.sojuNetworkId == id)
                        toggle(*manager, "Attach this network locally", child.sojuEnabled,
                               [this, name = child.name](bool enabled) {
                                   model.actions.push_back(
                                       {"soju-set-enabled", name, enabled ? "1" : "0", {}});
                               });
            }
        }
        button(*manager, "New remote network form", [this] {
            sojuEditId.clear();
            sojuEditMode = false;
            sojuEditAttributes.clear();
            sojuEditChanges.clear();
            rebuildPage();
        });
        label(*manager,
              !sojuEditMode ? "Create remote network" : "Edit remote network [" + sojuEditId + "]",
              true);
        for (const char *key : {"name", "host", "port", "nickname", "username", "realname", "pass",
                                "sasl", "sasl-username", "sasl-password"}) {
            const auto value = sojuEditAttributes.find(key);
            text(
                *manager, std::string("Remote ") + key,
                value == sojuEditAttributes.end() ? "" : value->second,
                [this, key = std::string(key)](const std::string &next) {
                    sojuEditAttributes[key] = next;
                    sojuEditChanges[key] = next;
                },
                std::string_view(key) == "pass" || std::string_view(key) == "sasl-password");
        }
        const auto tls = sojuEditAttributes.find("tls");
        toggle(*manager, "Remote TLS", tls == sojuEditAttributes.end() || tls->second == "1",
               [this](bool enabled) {
                   sojuEditAttributes["tls"] = enabled ? "1" : "0";
                   sojuEditChanges["tls"] = enabled ? "1" : "0";
               });
        label(*manager, "Blank untouched password fields preserve existing remote passwords. "
                        "Editing a password to empty explicitly clears it. Server validation and "
                        "acknowledgements determine the result; sending a request is not success.");
        auto *submit = button(
            *manager, !sojuEditMode ? "Create remote network" : "Apply remote changes",
            [this, parent] {
                const auto current = model.networks.find(parent);
                if (sojuEditMode && (current == model.networks.end() ||
                                     !current->second.sojuNetworks.contains(sojuEditId))) {
                    setFeedback("The selected remote network is unavailable. No request was sent.");
                    return;
                }
                Action request;
                request.type = !sojuEditMode ? "soju-add-network" : "soju-change-network";
                request.buffer = parent;
                request.text = sojuEditId;
                request.attributes = sojuEditChanges;
                if (!sojuEditMode && !request.attributes.contains("tls"))
                    request.attributes["tls"] = "1";
                model.actions.push_back(std::move(request));
            },
            ButtonVariant::Primary);
        sojuSubmit = submit;
        submit->setEnabled(capable && !found->second.sojuMutationPending &&
                           (!sojuEditMode || found->second.sojuNetworks.contains(sojuEditId)));
        sojuMutationButtons.push_back(submit);
    }

    std::size_t resolveServer(const std::shared_ptr<std::string> &source) {
        const auto found = std::find_if(model.servers.begin(), model.servers.end(),
                                        [&](const auto &entry) { return entry.name == *source; });
        if (found == model.servers.end()) {
            setFeedback("This network profile was removed. Select another saved network.");
            return model.servers.size();
        }
        return std::size_t(found - model.servers.begin());
    }

    template <class Callback>
    auto onServer(std::shared_ptr<std::string> source, Callback callback) {
        return [this, source = std::move(source), callback = std::move(callback)](auto &&...args) {
            const auto index = resolveServer(source);
            if (index < model.servers.size())
                callback(index, std::forward<decltype(args)>(args)...);
        };
    }

    void connection(bool first) {
        ensureIdentity();
        if (first && model.servers.empty())
            model.servers.emplace_back();
        auto *profiles = first ? page : section("Saved networks");
        if (!first) {
            if (!model.servers.empty()) {
                serverIndex = std::min(serverIndex, model.servers.size() - 1);
                auto labels = networkLabels();
                networkPicker =
                    select(*profiles, "Network", std::move(labels), {},
                           [this](std::size_t index, std::string_view) {
                               if (index >= networkOptions.size())
                                   return;
                               const auto key = networkOptions[index].key;
                               const auto selected = std::find_if(
                                   model.servers.begin(), model.servers.end(),
                                   [&](const auto &entry) { return entry.name == key; });
                               if (selected != model.servers.end()) {
                                   serverIndex = std::size_t(selected - model.servers.begin());
                                   rebuildPage();
                               }
                           });
                networkPicker->setSelectedIndexSilently(serverIndex);
            }
            button(*profiles, "Add network", [this] {
                addServer();
                rebuildPage();
            });
            button(*profiles, "Add soju account", [this] {
                addServer();
                auto &account = model.servers.back();
                account.sojuAccount = true;
                account.bouncer = "soju";
                account.host.clear();
                account.sasl = "PLAIN";
                rebuildPage();
            });
        }
        if (model.servers.empty()) {
            label(*profiles,
                  "No networks saved. Add a network to connect, or keep using BlastIRC offline.");
            return;
        }
        serverIndex = std::min(serverIndex, model.servers.size() - 1);
        const std::size_t si = serverIndex;
        auto &server = model.servers[si];
        auto source = std::make_shared<std::string>(server.name);
        displayedServer = source;
        if (server.sojuAccount || !server.sojuParent.empty()) {
            connectionStatus = label(*profiles, "");
            updateConnectionStatus(server);
        }
        if (!server.sojuParent.empty()) {
            label(*profiles,
                  "Managed by soju account: " + std::string(networkParentCaption(server)));
            toggle(*profiles, "Attach this network locally", server.sojuEnabled,
                   [this, name = server.name](bool enabled) {
                       model.actions.push_back({"soju-set-enabled", name, enabled ? "1" : "0", {}});
                   });
            button(*profiles, "Manage soju account",
                   [this, parent = server.sojuParent] { open("manage-soju:" + parent); });
            label(*profiles, "Disabling local attachment does not delete or disconnect the "
                             "remote upstream network. Saved local conversations are retained.");
            return;
        }
        auto found = std::find_if(model.identities.begin(), model.identities.end(),
                                  [&](const auto &id) { return id.name == server.identity; });
        const std::size_t ii = std::size_t(found - model.identities.begin());
        auto *endpoint = first ? page : section("Network endpoint");
        if (!server.sojuAccount) {
            auto *profileName = text(*endpoint, "Network name", server.name, {});
            profileName->setOnChange(onServer(
                source, [this, source, profileName](std::size_t si, const std::string &value) {
                    const bool unique =
                        !value.empty() && std::none_of(model.servers.begin(), model.servers.end(),
                                                       [&](const auto &entry) {
                                                           return &entry != &model.servers[si] &&
                                                                  entry.name == value;
                                                       });
                    profileName->setInvalid(!unique);
                    if (!unique) {
                        invalidFields.insert(profileName);
                        return;
                    }
                    invalidFields.erase(profileName);
                    model.servers.at(si).name = value;
                    *source = value;
                    if (networkPicker) {
                        networkPicker->setOptions(networkLabels());
                        networkPicker->setSelectedIndexSilently(si);
                    }
                    updateIdentityScope();
                }));
        }
        text(*endpoint, server.sojuAccount ? "Account alias" : "Display alias", server.alias,
             onServer(source, [this](std::size_t si, const std::string &value) {
                 model.servers.at(si).alias = value;
             }));
        text(*endpoint, "Avatar image path", server.avatar,
             onServer(source, [this](std::size_t si, const std::string &value) {
                 model.servers.at(si).avatar = value;
             }));
        label(*endpoint, server.sojuAccount
                             ? "Account alias changes its display name, not its stable profile key "
                               "or saved conversations."
                             : "Alias and local avatar image change the server rail only; "
                               "the network name identifies saved conversations.");
        text(*endpoint, "Server address", server.host,
             onServer(source, [this](std::size_t si, const std::string &value) {
                 model.servers.at(si).host = value;
             }));
        auto *port = text(*endpoint, "Port (1–65535)", std::to_string(server.port), {});
        port->setOnChange(onServer(source, [this, port](std::size_t si, const std::string &value) {
            int number = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
            const bool valid = result.ec == std::errc{} &&
                               result.ptr == value.data() + value.size() && number >= 1 &&
                               number <= 65535;
            port->setInvalid(!valid);
            if (valid) {
                invalidFields.erase(port);
                model.servers.at(si).port = number;
            } else
                invalidFields.insert(port);
        }));
        auto *tlsWarning =
            label(*endpoint, "Plaintext exposes messages and credentials to the network.");
        tlsWarning->setColor(colorSpecFromRole(ColorRole::Error));
        tlsWarning->setVisible(!server.tls);
        toggle(*endpoint, "TLS (recommended)", server.tls,
               onServer(source, [this, port, tlsWarning](std::size_t si, bool value) {
                   auto &selected = model.servers.at(si);
                   selected.tls = value;
                   if (value && selected.port == 6667) {
                       selected.port = 6697;
                       port->setValue("6697");
                       port->setInvalid(false);
                       invalidFields.erase(port);
                   }
                   tlsWarning->setVisible(!value);
               }));
        if (!server.sojuAccount) {
            text(*endpoint, "Server password (this network only)", server.password,
                 onServer(source,
                          [this](std::size_t si, const std::string &value) {
                              model.servers.at(si).password = value;
                          }),
                 true);
            label(*endpoint, "This server password is separate from shared identity credentials. "
                             "It is stored in plain text in your private configuration file.");
        }
        auto *credentials = first ? page
                                  : section(server.sojuAccount ? "Soju account authentication"
                                                               : "Network authentication");
        if (!server.sojuAccount) {
            select(
                *credentials, "Authentication method", {"None", "PLAIN", "EXTERNAL"},
                server.sasl.empty() ? "None" : server.sasl,
                onServer(source, [this](std::size_t si, std::size_t index, std::string_view value) {
                    model.servers.at(si).sasl = index == 0 ? "" : std::string(value);
                }));
            label(*credentials, "PLAIN requires TLS and uses the selected identity's password. "
                                "EXTERNAL uses that identity's certificate and private key.");
        }
        auto *identity = first ? page : section("Shared identity");
        networkIdentityPicker =
            select(*identity, "Shared identity profile", identityNames(), server.identity,
                   onServer(source, [this](std::size_t si, std::size_t index, std::string_view) {
                       if (index < model.identities.size()) {
                           model.servers.at(si).identity = model.identities[index].name;
                           rebuildPage();
                       }
                   }));
        if (found != model.identities.end()) {
            identityScope = label(*identity, "");
            identityScopes.emplace_back(identityScope, ii);
            if (server.sojuAccount) {
                sharedIdentityText(*identity, "Account username", ii, &Identity::username);
                auto *password = sharedIdentityText(*identity, "Account password", ii,
                                                    &Identity::password, true);
                auto *controlPass =
                    text(*identity, "Control connection PASS (management only)", server.password,
                         onServer(source,
                                  [this](std::size_t si, const std::string &value) {
                                      model.servers.at(si).password = value;
                                  }),
                         true);
                auto *authHelp = label(*identity, "");
                const auto updateAuth = [password, controlPass, authHelp](std::string_view auth) {
                    password->setEnabled(auth == "PLAIN");
                    controlPass->parent()->setVisible(auth.empty());
                    controlPass->parent()->setParticipatesInLayout(auth.empty());
                    authHelp->setText(
                        auth == "PLAIN"
                            ? "Password authentication requires TLS. Account credentials are "
                              "stored in plain text in your private configuration file."
                        : auth == "EXTERNAL"
                            ? "Set the selected identity's client certificate and private key "
                              "using Edit identity."
                            : "Management only: no SASL authentication. Control connection PASS "
                              "does not attach managed networks; choose PLAIN or EXTERNAL to "
                              "attach them.");
                };
                auto *auth =
                    select(*credentials, "Authentication method",
                           {"Password (SASL PLAIN)", "Client certificate (EXTERNAL)",
                            "Management only (no SASL)"},
                           {},
                           onServer(source, [this, updateAuth](std::size_t si, std::size_t index,
                                                               std::string_view) {
                               auto &selected = model.servers.at(si);
                               selected.sasl = index == 0 ? "PLAIN" : index == 1 ? "EXTERNAL" : "";
                               updateAuth(selected.sasl);
                           }));
                if (server.sasl.empty() || server.sasl == "PLAIN" || server.sasl == "EXTERNAL")
                    auth->setSelectedIndexSilently(server.sasl == "PLAIN"      ? 0
                                                   : server.sasl == "EXTERNAL" ? 1
                                                                               : 2);
                updateAuth(server.sasl);
                label(*identity, "These credentials belong to the selected shared identity and "
                                 "affect every profile listed above. Save and reconnect to apply "
                                 "account or authentication changes.");
            }
            if (first && !server.sojuAccount)
                identityFields(*identity, ii);
            else {
                label(*identity, server.sojuAccount
                                     ? "Edit nickname, client certificate and private key in "
                                       "Identities."
                                     : "Nickname, username and authentication credentials are "
                                       "shared by every network listed above. Edit them in "
                                       "Identities.");
                button(*identity, "Edit identity", [this, ii] {
                    const bool different = identityIndex != ii;
                    identityIndex = ii;
                    chooseTab(Tab::Identities);
                    if (different)
                        rebuildPage(false);
                    focusFirst();
                });
            }
        } else {
            label(*identity, "This identity is unavailable. Choose an existing profile before "
                             "connecting.")
                ->setColor(colorSpecFromRole(ColorRole::Error));
        }
        auto *startup = first ? page : section("Channels and bouncer");
        text(*startup, "Auto-join channels", joined(server.channels),
             onServer(source, [this](std::size_t si, const std::string &value) {
                 model.servers.at(si).channels = words(value);
             }));
        label(*startup, "Comma-separated, e.g. #linux, #friends");
        select(*startup, "Bouncer", {"none", "soju", "znc"}, server.bouncer,
               onServer(source, [this](std::size_t si, std::size_t, std::string_view value) {
                   model.servers.at(si).bouncer = value;
               }));
        if (server.sojuAccount) {
            toggle(*startup, "Connect automatically", server.sojuEnabled,
                   onServer(source, [this](std::size_t si, bool enabled) {
                       model.servers.at(si).sojuEnabled = enabled;
                   }));
            label(*startup, "Connect this account and its enabled managed networks at startup. "
                            "Save to keep this setting; reconnect to apply account changes now.");
            text(*startup, "Trusted soju file-host hostname (optional)", server.sojuTrustedFileHost,
                 onServer(source, [this](std::size_t si, const std::string &value) {
                     model.servers.at(si).sojuTrustedFileHost = value;
                 }));
            label(*startup,
                  "FILEHOST sends this account's actual username and password to the upload host. "
                  "Trusting another hostname grants it those credentials. A configured password "
                  "is required, including with EXTERNAL authentication.");
        } else if (server.bouncer.starts_with("soju")) {
            button(
                *startup, "Convert to soju account", onServer(source, [this, source](std::size_t) {
                    confirm("convert-soju", "Convert profile to soju account?",
                            "Use this exact endpoint and shared identity as the account control "
                            "session? "
                            "Existing local conversation history and profile key are retained. A "
                            "legacy "
                            "username suffix is not treated as a remote network ID.",
                            "Convert account", onServer(source, [this](std::size_t si) {
                                auto &entry = model.servers.at(si);
                                const auto identity =
                                    std::find_if(model.identities.begin(), model.identities.end(),
                                                 [&](const auto &profile) {
                                                     return profile.name == entry.identity;
                                                 });
                                if (identity == model.identities.end() ||
                                    identity->username.find('/') != std::string::npos) {
                                    setFeedback(
                                        "Choose a base-account identity with no /network selector "
                                        "before conversion. Create a separate identity rather than "
                                        "changing credentials shared by other profiles.");
                                    return;
                                }
                                entry.sojuAccount = true;
                                entry.bouncer = "soju";
                                rebuildPage();
                            }));
                }));
            sojuAssociation(server.name, *startup);
        }
        auto *actions = row(*page);
        connectButton = button(*actions, "Save and connect",
                               onServer(source, [this](std::size_t si) { connect(si); }),
                               ButtonVariant::Primary);
        label(*page, "Saves the current network and shared identity settings before connecting. "
                     "If saving fails, the current connection is kept and the error is shown "
                     "here. Appearance edits do not reconnect.");
        if (!first) {
            button(*actions, "Disconnect", onServer(source, [this](std::size_t si) {
                model.actions.push_back(
                    {"disconnect", model.servers.at(si).name + "/*server*", {}, {}});
            }));
            button(*actions, server.sojuAccount ? "Remove local soju account…" : "Remove network…",
                   onServer(
                       source,
                       [this, source](std::size_t si) {
                           const std::string name = model.servers.at(si).name;
                           confirm(
                               "remove-network", "Remove network?",
                               "Remove '" + name +
                                   "' from saved networks and disconnect it? This removes local "
                                   "configuration only, not remote networks. Saved history, drafts "
                                   "and recovery are kept.",
                               "Remove network", onServer(source, [this](std::size_t si) {
                                   if (model.servers[si].sojuAccount) {
                                       model.actions.push_back(
                                           {"soju-remove-account", model.servers[si].name, {}, {}});
                                   } else {
                                       model.actions.push_back(
                                           {"disconnect",
                                            model.servers[si].name + "/*server*",
                                            {},
                                            {}});
                                       model.servers.erase(model.servers.begin() + si);
                                   }
                                   serverIndex = 0;
                                   rebuildPage();
                               }));
                       }),
                   ButtonVariant::Destructive);
        }
        if (server.sojuAccount)
            sojuManagement(server.name);
    }

    void identities() {
        ensureIdentity();
        identityIndex = std::min(identityIndex, model.identities.size() - 1);
        auto *profiles = section("Saved identities");
        identityPicker =
            select(*profiles, "Profile", identityNames(), model.identities[identityIndex].name,
                   [this](std::size_t index, std::string_view) {
                       if (index < model.identities.size()) {
                           identityIndex = index;
                           rebuildPage();
                       }
                   });
        button(*profiles, "Add identity", [this] {
            addIdentity();
            rebuildPage();
        });
        const std::size_t ii = identityIndex;
        const auto &identity = model.identities[ii];
        identityScope = label(*profiles, "");
        identityScopes.emplace_back(identityScope, ii);
        label(*profiles, "Changes to this identity affect every network listed above. "
                         "Network-specific server passwords stay in Networks.");
        auto *details = section("Profile details");
        text(*details, "Profile name", identity.name, [this, ii](const std::string &value) {
            const std::string old = model.identities.at(ii).name;
            model.identities.at(ii).name = value;
            for (auto &server : model.servers)
                if (server.identity == old)
                    server.identity = value;
            identityPicker->setOptions(identityNames());
            identityPicker->setSelectedIndexSilently(ii);
            if (networkIdentityPicker && serverIndex < model.servers.size()) {
                networkIdentityPicker->setOptions(identityNames());
                const auto &selected = model.servers[serverIndex].identity;
                const auto names = identityNames();
                const auto found = std::find(names.begin(), names.end(), selected);
                if (found != names.end())
                    networkIdentityPicker->setSelectedIndexSilently(
                        std::size_t(found - names.begin()));
            }
            updateIdentityScope();
        });
        identityFields(*details, ii);
        auto *remove = button(
            *page, "Remove identity…",
            [this, ii] {
                const std::string name = model.identities.at(ii).name;
                confirm("remove-identity", "Remove identity?",
                        "Remove '" + name +
                            "'? Networks using it will use the first remaining identity.",
                        "Remove identity", [this, ii] {
                            if (ii >= model.identities.size() || model.identities.size() <= 1)
                                return;
                            const std::string name = model.identities[ii].name;
                            model.identities.erase(model.identities.begin() + ii);
                            for (auto &server : model.servers)
                                if (server.identity == name)
                                    server.identity = model.identities.front().name;
                            identityIndex = 0;
                            chooseTab(Tab::Servers);
                            rebuildPage(false);
                            chooseTab(Tab::Identities);
                            rebuildPage();
                        });
            },
            ButtonVariant::Destructive);
        remove->setEnabled(model.identities.size() > 1);
    }

    void save(std::string network = {}, bool closeOnSuccess = false) {
        if (pendingSaveRequest)
            return;
        execute("persist");
        pendingSaveRequest = ++model.settingsSaveRequest;
        closeAfterSave = closeOnSuccess;
        saveFeedbackActive = true;
        model.actions.push_back({network.empty() ? "save" : "save-connect",
                                 {},
                                 std::move(network),
                                 {},
                                 pendingSaveRequest});
        setFeedback("");
        if (saveButton)
            saveButton->setEnabled(false);
        if (connectButton)
            connectButton->setEnabled(false);
    }

    void saveSettings() {
        if (pendingSaveRequest)
            return;
        if (!invalidFields.empty()) {
            setFeedback("Correct the highlighted fields before saving.");
            return;
        }
        for (std::size_t i = 0; i < model.servers.size(); ++i)
            if (auto error = validateServerProfile(model, i); !error.empty()) {
                setFeedback(model.servers[i].name + ": " + error);
                return;
            }
        for (std::size_t i = 0; i < model.identities.size(); ++i)
            if (auto error = validateIdentityProfile(model, i); !error.empty()) {
                setFeedback(model.identities[i].name + ": " + error);
                return;
            }
        save();
    }

    void updateTopic() {
        const auto found = model.buffers.find(topicBuffer);
        if (found == model.buffers.end() || found->second.kind != "channel" ||
            !found->second.joined) {
            setFeedback("Join a channel before changing its topic.");
            return;
        }
        if (editedTopic.find_first_of("\r\n") != std::string::npos ||
            editedTopic.find('\0') != std::string::npos) {
            setFeedback("Topics cannot contain NUL or line breaks.");
            return;
        }
        model.actions.push_back(
            {"command", topicBuffer, "/topic " + found->second.target + " :" + editedTopic, {}});
        dismiss();
    }

    void search() {
        if (searchQuery.empty()) {
            setFeedback("Enter text to search for.");
            return;
        }
        if (searchBuffer.empty() || !model.buffers.contains(searchBuffer)) {
            setFeedback("Choose an existing conversation before searching.");
            return;
        }
        model.actions.push_back({"search", searchBuffer, searchQuery, {}});
        dismiss();
    }

    void makeConfirmation() {
        auto background = std::make_unique<Box>();
        confirmPanel = background.get();
        confirmPanel->setDialogStyle();
        track(confirmPanel, MetricKind::Panel);
        confirmPanel->setClipChildren(true);
        confirmPanel->setZIndex(3);
        auto content = column(14);
        confirmChrome = content.get();
        padding(*content, 20);
        confirmTitle = label(*content, "", true);
        auto viewport = std::make_unique<ScrollView>();
        track(viewport.get(), MetricKind::Scroll);
        viewport->setFlexGrow(1);
        viewport->setFillWidth(true);
        viewport->setFillHeight(true);
        viewport->content()->setDirection(FlexDirection::Vertical);
        viewport->content()->setAlign(FlexAlign::Stretch);
        track(viewport->content(), MetricKind::Flex);
        confirmMessage = label(*viewport->content(), "");
        content->addChild(std::move(viewport));
        auto *actions = row(*content);
        confirmCancel = button(*actions, "Cancel", [this] { hideConfirmation(); });
        confirmAccept = button(
            *actions, "Confirm",
            [this] {
                auto accepted = std::move(confirmAction);
                hideConfirmation();
                if (accepted)
                    accepted();
            },
            ButtonVariant::Destructive);
        confirmPanel->addChild(std::move(content));
        confirmPanel->setVisible(false);
        owner.addChild(std::move(background));
    }

    void confirm(std::string name, std::string_view title, std::string_view message,
                 std::string_view accept, std::function<void()> callback) {
        if (mode.empty() && confirmation.empty() && dispatcher)
            returnFocus = dispatcher->captureTabFocus();
        confirmation = std::move(name);
        confirmAction = std::move(callback);
        confirmTitle->setText(title);
        confirmMessage->setText(message);
        confirmAccept->setText(accept);
        confirmReturnFocus = dispatcher ? dispatcher->focusedArea() : nullptr;
        if (panel)
            panel->setVisible(false);
        confirmPanel->setVisible(true);
        showOverlay();
        if (dispatcher)
            dispatcher->setFocus(confirmCancel->inputArea());
    }

    void restoreReturnFocus() {
        if (!dispatcher)
            return;
        const bool hadFocus = returnFocus.key || returnFocus.fallbackKey || returnFocus.index;
        dispatcher->setFocus(nullptr);
        dispatcher->restoreTabFocus(returnFocus);
        if (hadFocus && !dispatcher->focusedArea() && model.buffers.contains(model.activeBuffer))
            execute("buffer:" + model.activeBuffer);
    }

    void hideConfirmation() {
        if (confirmation.empty())
            return;
        confirmation.clear();
        confirmAction = {};
        confirmPanel->setVisible(false);
        if (!mode.empty() && panel) {
            panel->setVisible(true);
            if (dispatcher)
                dispatcher->setFocus(confirmReturnFocus);
        } else {
            owner.setVisible(false);
            owner.setHitTestVisible(false);
            owner.setExcludeSubtreeFromTabOrder(true);
            restoreReturnFocus();
        }
        confirmReturnFocus = nullptr;
    }

    void clearHistory() {
        confirm(
            "clear-history", "Delete local history?",
            "Permanently delete locally saved messages? Server and bouncer history are unaffected.",
            "Delete local history",
            [this] { model.actions.push_back({"clear_history", {}, {}, {}}); });
    }

    Input *profileField(Flex &parent, std::string_view caption, const std::string &key) {
        auto *field = text(parent, caption, "", {});
        field->setClearButtonEnabled(false);
        field->setEditValidator([](std::string_view, std::string_view, bool) { return false; });
        field->inputArea()->setTabFocusKey(key);
        if (caption == "Details" || caption == "Channels" || caption == "Error" ||
            caption == "Away" || caption == "Real name" || caption == "Server information") {
            field->setMultiline(true);
            metrics.back().kind = MetricKind::ProfileInput;
        }
        return field;
    }

    void profileValue(Input &field, const std::string &value) {
        const bool visible = !value.empty();
        auto *unit = field.parent();
        unit->setVisible(visible);
        unit->setParticipatesInLayout(visible);
        unit->setExcludeSubtreeFromTabOrder(!visible);
        if (!visible && dispatcher && dispatcher->focusedArea() == field.inputArea())
            dispatcher->setFocus(dispatcher->firstTabFocusUnder(panel));
        if (field.value() != value)
            field.setValue(value);
    }

    void updateProfile() {
        const auto &profile = model.whoisProfile;
        profileValue(*profileLoading, profile.loading ? "Requesting " + profile.target + "…" : "");
        profileValue(*profileError, profile.error);
        const auto number = [](const std::string &text) -> std::optional<std::uint64_t> {
            std::uint64_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return std::nullopt;
            return value;
        };
        static constexpr const char *captions[] = {
            "Nick",    "Username", "Host",   "Real name",          "Away",     "Idle",
            "Sign-on", "Account",  "Server", "Server information", "Channels", "Details"};
        static constexpr const char *keys[] = {"nick",   "username",    "host",     "real-name",
                                               "away",   "idle",        "signon",   "account",
                                               "server", "server-info", "channels", "details"};
        static constexpr std::size_t groups[] = {0, 0, 0, 0, 1, 1, 1, 2, 3, 3, 4, 1};
        static constexpr const char *headings[] = {"Identity", "Status", "Account", "Server",
                                                   "Channels"};
        for (std::size_t index = 0; index < profile.records.size(); ++index) {
            const auto &record = profile.records[index];
            if (index == profileCards.size()) {
                ProfileCard card;
                auto body = column(12);
                card.body = body.get();
                page->addChild(std::move(body));
                auto *header = row(*card.body, false);
                auto avatar = column(0, MetricKind::Section);
                padding(*avatar, 10);
                card.avatar = label(*avatar, "", true);
                header->addChild(std::move(avatar));
                card.avatar->setTooltip("Generated user avatar");
                card.nick = label(*header, "", true);
                for (std::size_t group = 0; group < card.groups.size(); ++group) {
                    auto sectionBody = column(8, MetricKind::Section);
                    card.groups[group] = sectionBody.get();
                    label(*sectionBody, headings[group], true);
                    card.body->addChild(std::move(sectionBody));
                }
                for (std::size_t field = 0; field < card.fields.size(); ++field)
                    card.fields[field] =
                        profileField(*card.groups[groups[field]], captions[field],
                                     "blastirc.whois." + std::to_string(index) + "." + keys[field]);
                profileCards.push_back(card);
            }
            auto &card = profileCards[index];
            card.body->setVisible(true);
            card.body->setParticipatesInLayout(true);
            card.body->setExcludeSubtreeFromTabOrder(false);
            card.nick->setText(record.nick);
            card.nick->setColor(nickColor(model, record.nick, profile.server));
            card.avatar->setText(nativeAvatarInitials(record.nick));
            card.avatar->setColor(nickColor(model, record.nick, profile.server));
            std::string details;
            for (const auto &detail : record.details) {
                if (!details.empty())
                    details += '\n';
                details += detail;
            }
            std::string idle, signon;
            if (!record.idle.empty()) {
                if (const auto seconds = number(record.idle)) {
                    const auto part = [&](std::uint64_t count, const char *unit) {
                        if (count)
                            idle += std::to_string(count) + unit + " ";
                    };
                    part(*seconds / 86400, "d");
                    part(*seconds / 3600 % 24, "h");
                    part(*seconds / 60 % 60, "m");
                    idle += std::to_string(*seconds % 60) + " s";
                } else
                    idle = "Invalid duration";
            }
            if (!record.signon.empty()) {
                signon = "Invalid timestamp";
                if (const auto seconds = number(record.signon);
                    seconds && std::in_range<std::time_t>(*seconds)) {
                    const auto value = static_cast<std::time_t>(*seconds);
                    std::tm local{};
                    char text[128]{};
                    if (localtime_r(&value, &local) && local.tm_year >= -1899 &&
                        local.tm_year <= 8099 &&
                        std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %Z", &local))
                        signon = text;
                }
            }
            const std::array<const std::string *, 12> values = {
                &record.nick,     &record.username,
                &record.host,     &record.realName,
                &record.away,     &idle,
                &signon,          &record.account,
                &record.server,   &record.serverInfo,
                &record.channels, &details};
            std::array<bool, 5> visible{};
            for (std::size_t field = 0; field < values.size(); ++field) {
                profileValue(*card.fields[field], *values[field]);
                visible[groups[field]] |= !values[field]->empty();
            }
            for (std::size_t group = 0; group < visible.size(); ++group) {
                card.groups[group]->setVisible(visible[group]);
                card.groups[group]->setParticipatesInLayout(visible[group]);
                card.groups[group]->setExcludeSubtreeFromTabOrder(!visible[group]);
            }
        }
        for (std::size_t index = profile.records.size(); index < profileCards.size(); ++index) {
            profileCards[index].body->setVisible(false);
            profileCards[index].body->setParticipatesInLayout(false);
            profileCards[index].body->setExcludeSubtreeFromTabOrder(true);
        }
        profileRevision = profile.revision;
    }

    void open(std::string_view name) {
        if (name == "settings-uploads") {
            tab = Tab::Chat;
            beginPanel("settings", "Settings", true);
            rebuildPage();
            return;
        }
        const auto separator = name.find(':');
        if (separator != std::string_view::npos &&
            (name.starts_with("manage-soju:") || name.starts_with("add-soju-network:") ||
             name.starts_with("edit-soju-network:") || name.starts_with("delete-soju-network:"))) {
            const std::string target(name.substr(separator + 1));
            auto selected = std::find_if(model.servers.begin(), model.servers.end(),
                                         [&](const auto &entry) { return entry.name == target; });
            if (selected == model.servers.end())
                return;
            const auto id = selected->sojuNetworkId;
            const std::string parent =
                selected->sojuParent.empty() ? selected->name : selected->sojuParent;
            selected =
                std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &entry) {
                    return entry.name == parent && entry.sojuAccount;
                });
            if (selected == model.servers.end())
                return;
            serverIndex = std::size_t(selected - model.servers.begin());
            sojuEditParent = parent;
            sojuEditId.clear();
            sojuEditMode = name.starts_with("edit-soju-network:");
            sojuEditAttributes.clear();
            sojuEditChanges.clear();
            if (name.starts_with("edit-soju-network:")) {
                sojuEditId = id;
                const auto runtime = model.networks.find(parent);
                if (runtime != model.networks.end()) {
                    const auto remote = runtime->second.sojuNetworks.find(id);
                    if (remote != runtime->second.sojuNetworks.end()) {
                        sojuEditId = id;
                        sojuEditAttributes = remote->second.attributes;
                        sojuEditAttributes.erase("pass");
                        sojuEditAttributes.erase("sasl-password");
                    }
                }
            }
            tab = Tab::Servers;
            beginPanel("settings", "Settings", true);
            rebuildPage();
            if (name.starts_with("delete-soju-network:") && !id.empty())
                deleteSojuNetwork(parent, id);
            return;
        }
        if (name == "clear-history") {
            clearHistory();
            return;
        }
        if (name.starts_with("server:")) {
            const auto target = name.substr(7);
            const auto found =
                std::find_if(model.servers.begin(), model.servers.end(),
                             [&](const auto &server) { return server.name == target; });
            if (found != model.servers.end())
                serverIndex = std::size_t(found - model.servers.begin());
            tab = Tab::Servers;
            beginPanel("settings", "Settings", true);
            rebuildPage();
            if (found == model.servers.end())
                setFeedback("No saved network named '" + std::string(target) + "'.");
            return;
        }
        if (name == "add-server") {
            addServer();
            tab = Tab::Servers;
            beginPanel("settings", "Settings", true);
        } else if (name == "networks") {
            tab = Tab::Servers;
            beginPanel("settings", "Settings", true);
        } else if (name == "settings") {
            beginPanel("settings", "Settings", true);
        } else if (name == "first-run") {
            beginPanel("first-run", "Welcome to BlastIRC", false);
        } else if (name == "search") {
            searchQuery.clear();
            searchBuffer = model.activeBuffer;
            beginPanel("search", "Search conversation", false);
        } else if (name == "topic") {
            topicBuffer = model.activeBuffer;
            const auto found = model.buffers.find(topicBuffer);
            editedTopic = found == model.buffers.end() ? "" : found->second.topic;
            beginPanel("topic", "Channel topic", false);
        } else if (name.starts_with("nick-color:")) {
            nick = name.substr(11);
            beginPanel("nick-color", "Nick color: " + nick, false);
        } else if (name == "config-error") {
            if (model.status.starts_with("Configuration was not loaded;") ||
                model.status.starts_with("Configuration errors must be fixed before saving;"))
                configMessage = model.status;
            beginPanel("config-error", "Configuration not loaded", false);
        } else if (name == "recovery") {
            beginPanel("recovery", "Interrupted send recovery", false);
        } else {
            return;
        }
        rebuildPage();
    }

    void dismiss() {
        if (!confirmation.empty()) {
            hideConfirmation();
            return;
        }
        stopRecording(false);
        if (mode == "whois") {
            model.whoisProfile.open = false;
            model.whoisProfile.loading = false;
            ++model.whoisProfile.revision;
        }
        if (dispatcher)
            dispatcher->setFocus(nullptr);
        mode.clear();
        saveFeedbackActive = false;
        owner.setVisible(false);
        owner.setHitTestVisible(false);
        owner.setExcludeSubtreeFromTabOrder(true);
        restoreReturnFocus();
    }

    void sync(float width, float height, float scale) {
        const auto &profile = model.whoisProfile;
        if (mode == "whois" && !profile.open)
            dismiss();
        // Native focus loss cancels preedit; wait rather than discard an in-flight composition.
        const auto *focused = dispatcher ? dispatcher->focusedArea() : nullptr;
        const auto *native =
            focused ? dynamic_cast<const Input *>(focused->textInputClient()) : nullptr;
        const bool composing = native && native->composing();
        if (profile.open && !profile.requestId.empty() && (mode.empty() || mode == "whois") &&
            confirmation.empty() && !composing) {
            if (mode != "whois" || profileRequest != profile.requestId) {
                profileRequest = profile.requestId;
                beginPanel("whois", "WHOIS profile", false);
                rebuildPage();
            } else if (profileRevision != profile.revision) {
                updateProfile();
            }
        }
        if (recordingButton && dispatcher &&
            dispatcher->focusedArea() != recordingButton->inputArea())
            stopRecording(false);
        if (bindingConflicts && lastConflictBindings != model.settings.bindings) {
            lastConflictBindings = model.settings.bindings;
            std::string warning;
            for (auto first = model.settings.bindings.begin();
                 first != model.settings.bindings.end(); ++first) {
                for (auto second = std::next(first); second != model.settings.bindings.end();
                     ++second) {
                    if (!ui_logic::bindingConflicts(first->second, second->second))
                        continue;
                    if (!warning.empty())
                        warning += '\n';
                    warning += std::string(commandDisplayName(first->first)) + " / " +
                               std::string(commandDisplayName(second->first)) +
                               ": overlapping sequence in the composer";
                }
            }
            bindingConflicts->setText(warning);
            bindingConflicts->setVisible(!warning.empty());
            bindingConflicts->setParticipatesInLayout(!warning.empty());
        }
        retired.clear();
        zoom = std::max(.1f, scale);
        applyMetrics();
        owner.setSize(width, height);
        dim->setSize(width, height);
        blocker->setSize(width, height);
        if (themeStatus && lastThemeSource != themeSource()) {
            lastThemeSource = themeSource();
            themeStatus->setText("Active palette: " + lastThemeSource +
                                 " · Reduced motion / Effects Off disables transitions");
        }
        if (applicationStatus && lastApplicationStatus != model.status) {
            lastApplicationStatus = model.status;
            applicationStatus->setText(
                model.status.empty() ? "" : "Application feedback: " + model.status);
            applicationStatus->setVisible(!model.status.empty());
            applicationStatus->setParticipatesInLayout(!model.status.empty());
        }
        if (pendingSaveRequest && model.settingsSaveResult.requestId == pendingSaveRequest) {
            const auto &result = model.settingsSaveResult;
            pendingSaveRequest = 0;
            const bool close = std::exchange(closeAfterSave, false);
            if (saveFeedbackActive) {
                saveFeedbackActive = false;
                if (result.success && close)
                    dismiss();
                else
                    setFeedback(result.success ? "Save completed." : result.error, !result.success);
            }
        }
        if (saveButton)
            saveButton->setEnabled(!pendingSaveRequest);
        if (motionStatus)
            motionStatus->setText(
                model.settings.fx == FxLevel::Off
                    ? "Effective: event motion and event sounds are off. Physics values are kept."
                : model.settings.reducedMotion
                    ? "Effective: motion is disabled by Reduced motion. Enabled sound remains; "
                      "physics values are kept."
                    : "Effective: event motion is enabled; physics values below apply to events.");
        if (soundStatus)
            soundStatus->setText(
                model.settings.fx == FxLevel::Off ? "Effective: event sounds are muted by Effects "
                                                    "Off; your sound settings remain."
                : !model.settings.sound
                    ? "Effective: Sound enabled is off; volumes and cue preferences remain."
                : model.settings.masterVolume <= 0
                    ? "Effective: master volume is zero; enabled cues are silent."
                    : "Effective: enabled cues can play at the saved volumes. Reduced motion "
                      "does not mute sound.");
        if (cursorStatus)
            cursorStatus->setText(
                model.settings.cursorEffect == "off"
                    ? "Effective: static cursor only; cursor animation is Off."
                : model.settings.fx == FxLevel::Off
                    ? "Effective: cursor animation suppressed by Effects Off; shape stays."
                : model.settings.reducedMotion
                    ? "Effective: cursor animation suppressed by Reduced motion; shape stays."
                    : "Effective: selected animation follows the focused composer cursor.");
        bool serverSourcePresent = true;
        if (displayedServer) {
            const auto index = resolveServer(displayedServer);
            serverSourcePresent = index < model.servers.size();
            if (serverSourcePresent) {
                serverIndex = index;
                updateConnectionStatus(model.servers[index]);
            } else {
                if (connectButton)
                    connectButton->setEnabled(false);
                for (auto *control : sojuMutationButtons)
                    control->setEnabled(false);
            }
        }
        if (networkPicker &&
            !std::equal(networkOptions.begin(), networkOptions.end(), model.servers.begin(),
                        model.servers.end(), [this](const auto &option, const auto &profile) {
                            return option.key == profile.name &&
                                   option.caption == networkCaption(profile) &&
                                   option.parentCaption == networkParentCaption(profile);
                        })) {
            networkPicker->setOptions(networkLabels());
            if (serverSourcePresent && displayedServer)
                networkPicker->setSelectedIndexSilently(serverIndex);
            else
                networkPicker->clearSelection();
        }
        if (serverSourcePresent && connectButton && serverIndex < model.servers.size()) {
            const auto network = model.networks.find(model.servers[serverIndex].name);
            const bool reconnect = network != model.networks.end() &&
                                   network->second.state != "Disconnected" &&
                                   network->second.state != "Failed";
            connectButton->setText(reconnect ? "Save and reconnect" : "Save and connect");
            connectButton->setEnabled(!pendingSaveRequest);
        }
        if (serverSourcePresent && sojuStatus && serverIndex < model.servers.size()) {
            const auto runtime = model.networks.find(model.servers[serverIndex].name);
            const bool capable = runtime != model.networks.end() &&
                                 runtime->second.state == "Ready" &&
                                 runtime->second.caps.contains("soju.im/bouncer-networks");
            const bool pending =
                runtime != model.networks.end() && runtime->second.sojuMutationPending;
            if (runtime == model.networks.end())
                sojuStatus->setText("Connect this account to discover and manage remote networks.");
            else if (!runtime->second.sojuMutationError.empty())
                sojuStatus->setText("Request failed: " + runtime->second.sojuMutationError);
            else if (pending)
                sojuStatus->setText("Remote request pending acknowledgement.");
            else if (runtime->second.sojuMutationSucceeded)
                sojuStatus->setText(
                    "Remote request acknowledged by soju. Refresh displayed catalog.");
            else if (!capable)
                sojuStatus->setText("Remote management unavailable: " + runtime->second.state +
                                    ". The server must support soju.im/bouncer-networks.");
            else
                sojuStatus->setText(
                    runtime->second.sojuDiscoveryComplete
                        ? "Remote catalog received. Refresh displayed catalog after changes."
                        : "Waiting for the remote catalog.");
            for (auto *control : sojuMutationButtons)
                control->setEnabled(capable && !pending);
            const bool targetAvailable =
                !sojuEditMode || (runtime != model.networks.end() &&
                                  runtime->second.sojuNetworks.contains(sojuEditId));
            if (!targetAvailable) {
                if (sojuSubmit)
                    sojuSubmit->setEnabled(false);
                sojuStatus->setText(
                    "The selected remote network is unavailable. Editing is disabled; "
                    "no create request will be substituted.");
            }
        }
        if (fontFamily != themeUiFont().family || fontFeatures != themeUiFont().features) {
            fontFamily = themeUiFont().family;
            fontFeatures = themeUiFont().features;
            refreshFonts(owner);
        }
        const bool confirming = !confirmation.empty();
        Box *visible = confirming ? confirmPanel : panel;
        const bool showing = !mode.empty() || confirming;
        const std::string &dialog = confirming ? confirmation : mode;
        if (showing && visible) {
            const float preferredWidth = confirming ? 540.f : mode == "settings" ? 1000.f : 630.f;
            const float preferredHeight = confirming            ? 250.f
                                          : mode == "settings"  ? 720.f
                                          : mode == "first-run" ? 720.f
                                          : mode == "whois"     ? 720.f
                                          : mode == "search"    ? 270.f
                                                                : 400.f;
            const float w = std::min(preferredWidth * zoom, std::max(0.f, width - 24.f));
            const float verticalMargin = !confirming && mode == "whois" ? 48.f * zoom : 24.f;
            const float h =
                std::min(preferredHeight * zoom, std::max(0.f, height - verticalMargin));
            const float x = (width - w) * .5f, y = (height - h) * .5f;
            visible->setPosition(x, y);
            visible->setSize(w, h);
            (confirming ? confirmChrome : chrome)->setSize(w, h);
            if (!confirming && mode == "settings" && navigation && categoryPicker) {
                const bool compact = w < 760.f * zoom || h < 440.f * zoom;
                bool focusInNavigation = false, focusInPicker = false;
                if (dispatcher)
                    for (Node *node = dispatcher->focusedArea(); node; node = node->parent()) {
                        focusInNavigation |= node == navigation;
                        focusInPicker |= node == categoryPicker;
                    }
                chrome->setPadding((compact ? 8.f : 12.f) * zoom);
                chrome->setGap((compact ? 6.f : 8.f) * zoom);
                if (detailTitle) {
                    detailTitle->setVisible(!compact);
                    detailTitle->setParticipatesInLayout(!compact);
                }
                navigation->setVisible(!compact);
                categoryPicker->setVisible(compact);
                navigation->setParticipatesInLayout(!compact);
                categoryPicker->setParticipatesInLayout(compact);
                if (dispatcher && compact && focusInNavigation)
                    dispatcher->setFocus(dispatcher->firstTabFocusUnder(categoryPicker));
                else if (dispatcher && !compact && focusInPicker)
                    dispatcher->setFocus(categoryNavigation->focusArea());
            }
            if (geometryX != x || geometryY != y || geometryWidth != w || geometryHeight != h) {
                geometryX = x;
                geometryY = y;
                geometryWidth = w;
                geometryHeight = h;
                char rectangle[128];
                std::snprintf(rectangle, sizeof(rectangle), "%.3f %.3f %.3f %.3f", double(x),
                              double(y), double(w), double(h));
                if (lastGeometry != rectangle) {
                    lastGeometry = rectangle;
                    model.settings.workspace["native_dialog_rect"] = lastGeometry;
                }
            }
        }
        if (lastDialog != dialog) {
            lastDialog = dialog;
            model.settings.workspace["native_dialog"] = dialog;
        }
    }
};

Dialogs::Dialogs(Model &model, std::function<void(std::string)> execute)
    : impl_(std::make_unique<Impl>(*this, model, std::move(execute))) {}
Dialogs::~Dialogs() {
    if (impl_->dispatcher)
        impl_->dispatcher->setFocus(nullptr);
    while (!children().empty())
        removeChild(children().back().get());
}
void Dialogs::open(std::string_view name) { impl_->open(name); }
void Dialogs::remapBuffer(std::string_view oldId, std::string_view newId) {
    auto &s = *impl_;
    for (auto *key : {&s.returnFocus.key, &s.returnFocus.fallbackKey})
        if (*key)
            ui_logic::remapBufferFocusKey(**key, oldId, newId);
    if (s.searchBuffer == oldId)
        s.searchBuffer = newId;
    if (s.topicBuffer == oldId)
        s.topicBuffer = newId;
    if (s.mode == "topic" && s.topicTitle && s.topicBuffer == newId) {
        const auto found = s.model.buffers.find(s.topicBuffer);
        if (found != s.model.buffers.end())
            s.topicTitle->setText(found->second.target + " · " + found->second.server);
    }
}
void Dialogs::remapPane(std::uint64_t oldId, std::uint64_t newId) {
    for (auto *key : {&impl_->returnFocus.key, &impl_->returnFocus.fallbackKey})
        if (*key)
            ui_logic::remapPaneFocusKey(**key, oldId, newId);
}
void Dialogs::dismiss() { impl_->dismiss(); }
bool Dialogs::active() const { return !impl_->mode.empty() || !impl_->confirmation.empty(); }
bool Dialogs::settingsOpen() const {
    return impl_->mode == "settings" && impl_->confirmation.empty();
}
void Dialogs::sync(float width, float height, float scale) { impl_->sync(width, height, scale); }
void Dialogs::setInputDispatcher(InputDispatcher &dispatcher) {
    impl_->dispatcher = &dispatcher;
    if (active())
        impl_->focusFirst();
}
bool Dialogs::handleKey(const InputArea::KeyData &key) {
    if (!active() || !key.pressed || key.preedit)
        return false;
    if (impl_->recordingButton) {
        impl_->recordKey(key);
        return true;
    }
    const bool controlG =
        (key.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == KeyMod::Ctrl &&
        (key.sym == XKB_KEY_g || key.sym == XKB_KEY_G);
    if (key.sym == XKB_KEY_Escape || controlG) {
        dismiss();
        return true;
    }
    if (key.sym == XKB_KEY_Tab || key.sym == XKB_KEY_ISO_Left_Tab) {
        if (impl_->dispatcher) {
            impl_->dispatcher->cycleTabFocusInSubtree(this, key.sym == XKB_KEY_ISO_Left_Tab ||
                                                                (key.modifiers & KeyMod::Shift));
            if (impl_->scroll && impl_->confirmation.empty())
                impl_->scroll->requestScrollIntoView(impl_->dispatcher->focusedArea());
        }
        return true;
    }
    return false;
}
} // namespace blast
