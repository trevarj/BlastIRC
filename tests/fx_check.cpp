// Run: c++ -std=c++20 tests/fx_check.cpp -o /tmp/blastirc-check-fx && /tmp/blastirc-check-fx
#include "../src/fx/tween.hpp"
#include <cassert>
#include <cmath>
#include <limits>
#ifdef BLASTIRC_FX_INTEGRATION
// Like clipboard_check, compile this translation unit instead of effects.cpp.
#include "../src/fx/effects.cpp"

namespace blast {
class EffectsTestAccess {
  public:
    static void check() {
        Node root;
        root.setSize(128, 80);
        Effects effects;
        effects.attach(root);
        auto &p = *effects.impl;
        assert(p.mixer && p.audioError.empty());
        // Freeze the real dummy backend so observation never races clip completion.
        MIX_LockMixer(p.mixer);
        Model model;
        model.settings.pitchVariation = "off";
        const auto playing = [&] {
            std::size_t count = 0;
            for (const auto &voice : p.voices)
                count += MIX_TrackPlaying(voice.track);
            return count;
        };
        const auto findClip = [&](std::string_view name) {
            const auto clip = std::ranges::find(clipNames, name);
            assert(clip != std::end(clipNames));
            auto *audio = p.audio[clip - std::begin(clipNames)];
            assert(audio);
            return audio;
        };
        const auto expectClip = [&](std::string_view name, float gain = .5f) {
            assert(playing() == 1);
            for (const auto &voice : p.voices)
                if (MIX_TrackPlaying(voice.track)) {
                    assert(MIX_GetTrackAudio(voice.track) == findClip(name));
                    assert(MIX_GetTrackGain(voice.track) == gain);
                }
        };
        Event mention;
        mention.type = "mention";
        mention.server = "synthetic";
        mention.target = "#chat";
        mention.id = "row";
        model.now = 10;
        const auto checkMention = [&](bool ding) {
            p.stop();
            fxViewport(0, 0, 128, 80);
            fxAnchor(mention.server, mention.target, mention.id, 12, 30, 60, 12);
            model.effects = {mention};
            ++model.now;
            effects.draw(model);
            assert(p.count == 0);
            if (ding)
                expectClip("ding");
            else
                assert(playing() == 0);
        };
        for (auto level : {FxLevel::Full, FxLevel::Subtle}) {
            model.settings.fx = level;
            checkMention(true);
        }
        mention.history = true;
        checkMention(false);
        mention.history = false;
        model.settings.reducedMotion = true;
        checkMention(true);
        model.settings.reducedMotion = false;
        model.settings.cues["mention"].enabled = false;
        checkMention(false);
        model.settings.cues.erase("mention");
        model.settings.sound = false;
        checkMention(false);
        model.settings.sound = true;
        model.settings.fx = FxLevel::Off;
        checkMention(false);
        model.settings.fx = FxLevel::Full;
        mention.silent = true;
        checkMention(false);
        // A muted chat does not poison a later unrelated global cue.
        Event global;
        global.type = "state";
        global.text = "Disconnected";
        assert(!global.silent);
        model.effects = {global};
        model.now = 30;
        effects.draw(model);
        expectClip("power_down");
        p.stop();
        model.settings.reducedMotion = true;
        model.now = 31;
        effects.draw(model);
        expectClip("power_down");
        assert(p.count == 0);
        p.stop();
        model.settings.sound = false;
        model.now = 32;
        effects.draw(model);
        assert(playing() == 0);
        model.settings.sound = true;
        model.settings.fx = FxLevel::Off;
        model.now = 33;
        effects.draw(model);
        assert(playing() == 0 && p.count == 0);
        model.effects.clear();
        model.settings.fx = FxLevel::Full;
        const auto checkLive = [&](const char *type, std::string_view clip, float gain = .5f) {
            p.stop();
            model.effects.assign(1, Event{});
            model.effects.front().type = type;
            ++model.now;
            effects.draw(model);
            expectClip(clip, gain);
        };
        for (const auto &[event, soft, clear] :
             {std::array{"message", "tick", "tick_clear"},
              std::array{"send", "click", "click_clear"},
              std::array{"mention", "ding", "ding_clear"}}) {
            checkLive(event, soft);
            model.settings.cues[event].sound = CueSound::Clear;
            checkLive(event, clear);
        }
        // Legacy clip settings retain gain and the existing per-event Clear variant.
        model.settings.cues.erase("message");
        model.settings.cues["tick"] = {.volume = .3125f, .sound = CueSound::Clear};
        model.settings.cues["tick_clear"].enabled = false;
        checkLive("message", "tick_clear", .3125f);
        checkLive("notice", "tick", .3125f);
        model.settings.cues["tick"].sound = CueSound::Bubble;
        checkLive("message", "tick", .3125f);
        checkLive("notice", "tick", .3125f);
        model.settings.cues["tick"].sound = CueSound::Soft;
        checkLive("message", "tick", .3125f);
        model.settings.cues["notice"].sound = CueSound::Clear;
        checkLive("notice", "tick");
        p.stop();
        model.effects.clear();

        const auto drawPreview = [&](const char *event = "") {
            model.cuePreviewPending = event;
            ++model.now;
            effects.draw(model);
            assert(model.cuePreviewPending.empty() && playing() == 0);
        };
        drawPreview("bogus");
        assert(!p.previewTrack);
        auto &mentionCue = model.settings.cues["mention"];
        mentionCue = {.enabled = false, .volume = .375f, .pitch = .875f, .sound = CueSound::Clear};
        model.settings.masterVolume = .625f;
        model.settings.pitchVariation = "natural";
        drawPreview("mention");
        auto *preview = p.previewTrack;
        assert(preview && MIX_TrackPlaying(preview));
        assert(MIX_GetTrackAudio(preview) == findClip("ding_clear"));
        assert(MIX_GetTrackGain(preview) == .375f && MIX_GetMixerGain(p.mixer) == .625f);
        assert(MIX_GetTrackFrequencyRatio(preview) == .875f);
        assert(MIX_SetTrackPlaybackPosition(preview, 100));
        drawPreview();
        assert(MIX_TrackPlaying(preview) && MIX_GetTrackPlaybackPosition(preview) == 100);
        drawPreview("mention");
        assert(p.previewTrack == preview && MIX_GetTrackPlaybackPosition(preview) == 0);
        mentionCue.sound = CueSound::Soft;
        assert(MIX_SetTrackPlaybackPosition(preview, 100));
        drawPreview("mention");
        assert(p.previewTrack == preview && MIX_TrackPlaying(preview));
        assert(MIX_GetTrackAudio(preview) == findClip("ding"));
        assert(MIX_GetTrackPlaybackPosition(preview) == 0);
        const auto checkPreviewMute = [&](auto &setting, auto muted) {
            drawPreview("mention");
            assert(MIX_TrackPlaying(preview));
            const auto original = setting;
            setting = muted;
            drawPreview();
            assert(!MIX_TrackPlaying(preview));
            drawPreview("mention");
            assert(!MIX_TrackPlaying(preview));
            setting = original;
            drawPreview();
            assert(!MIX_TrackPlaying(preview));
        };
        checkPreviewMute(model.settings.sound, false);
        checkPreviewMute(model.settings.fx, FxLevel::Off);
        checkPreviewMute(model.settings.masterVolume, 0.f);
        checkPreviewMute(mentionCue.volume, 0.f);

        constexpr std::pair<CueSound, std::string_view> sharedClips[] = {
            {CueSound::Bubble, "bubble"},
            {CueSound::WoodTap, "wood-tap"},
            {CueSound::Marimba, "marimba"},
            {CueSound::Chime, "chime"},
        };
        for (const char *event : {"message", "send", "mention", "notice"}) {
            for (const auto &[sound, name] : sharedClips) {
                model.settings.cues[event] = {.sound = sound};
                checkLive(event, name);
                p.stop();
                model.effects.clear();
                drawPreview(event);
                assert(MIX_TrackPlaying(preview) && MIX_GetTrackAudio(preview) == findClip(name));
            }
        }
        p.stop();

        const auto drawTyping = [&](std::optional<TypingSound> tap = std::nullopt) {
            model.now += .1;
            model.typingSoundPending = tap;
            effects.draw(model);
            assert(!model.typingSoundPending && playing() == 0);
        };
        const auto expectSilent = [&] {
            assert(!p.composerTrack || !MIX_TrackPlaying(p.composerTrack));
        };
        drawTyping(TypingSound::Character);
        assert(!p.composerTrack);
        model.settings.typingSounds = true;
        model.settings.reducedMotion = true;
        model.settings.masterVolume = .4f;
        drawTyping();
        assert(!p.composerTrack);
        const std::pair<TypingSound, std::string_view> typingClips[] = {
            {TypingSound::Character, "keytap"},
            {TypingSound::Space, "keytap_space"},
            {TypingSound::DeleteCharacter, "keytap_delete"},
            {TypingSound::DeleteWord, "keytap_delete_word"},
        };
        MIX_Track *track = nullptr;
        for (const auto &[sound, name] : typingClips) {
            auto *audio = findClip(name);
            const auto duration = MIX_AudioFramesToMS(audio, MIX_GetAudioDuration(audio));
            assert(duration >= 20 && duration <= 50);
            for (int i = 0; i < 2; ++i) {
                drawTyping(sound);
                if (!track)
                    track = p.composerTrack;
                assert(track && p.composerTrack == track && MIX_TrackPlaying(track));
                assert(MIX_GetTrackAudio(track) == audio);
                assert(MIX_GetTrackGain(track) == .45f && MIX_GetMixerGain(p.mixer) == .4f);
                assert(MIX_GetTrackPlaybackPosition(track) == 0);
                // Simulate an in-flight tap before either a repeat or a clip switch.
                assert(MIX_SetTrackPlaybackPosition(track, 100));
                assert(MIX_GetTrackPlaybackPosition(track) == 100);
            }
        }
        drawTyping();
        assert(MIX_TrackPlaying(p.composerTrack)); // Idle frames leave the tap alone.
        model.settings.fx = FxLevel::Off;
        drawTyping(TypingSound::Space);
        expectSilent();
        model.settings.fx = FxLevel::Full;
        drawTyping();
        expectSilent();
        drawTyping(TypingSound::DeleteCharacter);
        model.settings.sound = false;
        drawTyping(TypingSound::DeleteWord);
        expectSilent();
        model.settings.sound = true;
        drawTyping();
        expectSilent();
        drawTyping(TypingSound::DeleteWord);
        model.settings.masterVolume = 0;
        drawTyping(TypingSound::Character);
        expectSilent();
        model.settings.masterVolume = .4f;
        drawTyping();
        expectSilent();
        drawTyping(TypingSound::Space);
        model.settings.typingSounds = false;
        drawTyping(TypingSound::DeleteCharacter);
        expectSilent();
        model.settings.typingSounds = true;
        drawTyping();
        expectSilent();
        MIX_UnlockMixer(p.mixer);
    }
};
} // namespace blast

static void effectsIntegrationCheck() {
    assert(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"));
    blast::EffectsTestAccess::check();
}
#endif
int main() {
    using namespace blast::fx;
    using blast::SendMotion;
    using blast::SendReveal;
    assert(cubicOut(0) == 0 && cubicOut(1) == 1);
    assert(std::abs(backOut(0)) < .00001f && std::abs(backOut(1) - 1) < .00001f);
    assert(bounceOut(0) == 0 && bounceOut(1) == 1);
    float previous = 0;
    for (int i = 0; i <= 1000; ++i) {
        const float t = float(i) / 1000;
        assert(cubicOut(t) >= previous);
        previous = cubicOut(t);
        assert(bounceOut(t) >= 0 && bounceOut(t) <= 1.00001f);
    }
    // Flight is opaque from the composer to the destination; cancellation settles immediately.
    const auto start = messageEntrance(0, true, 100, 500, 300, 100, true);
    const auto middle = messageEntrance(outgoingDuration / 2, true, 100, 500, 300, 100, true);
    assert(start.active && start.x == -200 && start.y == 400 && start.reveal == 1);
    assert(middle.active && std::abs(middle.x + 100) < .001f && std::abs(middle.y - 200) < .001f);
    for (bool own : {false, true}) {
        const double duration = own ? outgoingDuration : incomingDuration;
        assert(messageEntrance(duration * .8, own, 100, 500, 300, 100, true).active);
        for (double age : {-1., duration, duration + 10.}) {
            const auto settled = messageEntrance(age, own, 100, 500, 300, 100, true);
            assert(!settled.active && settled.x == 0 && settled.y == 0 && settled.reveal == 1);
        }
        const auto disabled = messageEntrance(.1, own, 100, 500, 300, 100, false);
        assert(!disabled.active && disabled.reveal == 1 && disabled.x == 0 && disabled.y == 0);
    }
    float previousScale = .94f;
    for (int step = 0; step <= 20; ++step) {
        const auto pop =
            messageEntrance(incomingDuration * (step / 20.), false, 100, 500, 300, 100, true);
        assert(pop.x == 0 && pop.y == 0 && pop.reveal == 1);
        assert(pop.active == (step < 20) && pop.scale >= previousScale && pop.scale <= 1);
        if (step == 0)
            assert(pop.scale == .94f);
        if (step == 10)
            assert(pop.scale > .98f);
        previousScale = pop.scale;
    }
    assert(previousScale == 1);
    const auto revealing =
        messageEntrance(.4, true, 100, 500, 300, 100, true, SendMotion::Pop, SendReveal::Fade);
    assert(revealing.active && revealing.scale == 1 && revealing.reveal < 1);
    for (auto motion : {SendMotion::None, SendMotion::Flight, SendMotion::Rise, SendMotion::Pop,
                        SendMotion::Slide})
        for (auto reveal : {SendReveal::None, SendReveal::Fade}) {
            const double lifetime = std::max(messageMotionDuration(motion),
                                             reveal == SendReveal::None ? 0 : sendRevealDuration);
            const auto sample = messageEntrance(.1, true, 100, 500, 300, 100, true, motion, reveal);
            assert(sample.active == (lifetime > .1));
            assert(sample.reveal ==
                   (reveal == SendReveal::None ? 1.f : float(.1 / sendRevealDuration)));
            if (motion == SendMotion::None)
                assert(sample.x == 0 && sample.y == 0 && sample.scale == 1);
            if (motion == SendMotion::Rise)
                assert(sample.x == 0 && sample.y > 0 && sample.y < 18 && sample.scale == 1);
            if (motion == SendMotion::Pop)
                assert(sample.x == 0 && sample.y == 0 && sample.scale > .92f);
            if (motion == SendMotion::Slide)
                assert(sample.x > 0 && sample.x < 24 && sample.y == 0 && sample.scale == 1);
            const auto settled =
                messageEntrance(lifetime, true, 100, 500, 300, 100, true, motion, reveal);
            assert(!settled.active && settled.x == 0 && settled.y == 0 && settled.scale == 1 &&
                   settled.reveal == 1);
            const auto disabled =
                messageEntrance(.1, true, 100, 500, 300, 100, false, motion, reveal);
            assert(!disabled.active && disabled.scale == 1 && disabled.reveal == 1);
            const auto received =
                messageEntrance(.1, false, 100, 500, 300, 100, true, motion, reveal);
            assert(received.active && received.x == 0 && received.y == 0 && received.scale < 1 &&
                   received.reveal == 1);
        }
    for (double age :
         {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        const auto invalid = messageEntrance(age, true, 100, 500, 300, 100, true);
        assert(!invalid.active && invalid.x == 0 && invalid.y == 0 && invalid.scale == 1 &&
               invalid.reveal == 1);
    }
    const auto assertSettled = [](const MessageEntrance &value) {
        assert(!value.active && value.x == 0 && value.y == 0 && value.scale == 1 &&
               value.reveal == 1);
    };
    // Incoming choices are independent of the composer choices and of each other.
    for (auto motion : {SendMotion::None, SendMotion::Rise, SendMotion::Pop, SendMotion::Slide})
        for (auto reveal : {SendReveal::None, SendReveal::Fade}) {
            for (float milliseconds : {100.f, 550.f, 2000.f}) {
                const double duration = messageIncomingDuration(milliseconds);
                const auto sample =
                    messageEntrance(duration / 2, false, 0, 0, 0, 0, true, SendMotion::Flight,
                                    SendReveal::Fade, motion, reveal, milliseconds);
                const auto alternate =
                    messageEntrance(duration / 2, false, 999, -999, 42, 24, true, SendMotion::None,
                                    SendReveal::None, motion, reveal, milliseconds);
                assert(sample.x == alternate.x && sample.y == alternate.y &&
                       sample.scale == alternate.scale && sample.reveal == alternate.reveal &&
                       sample.active == alternate.active);
                assert(sample.active == (motion != SendMotion::None || reveal != SendReveal::None));
                assert(sample.reveal == (reveal == SendReveal::None ? 1.f : .5f));
                if (motion == SendMotion::None)
                    assert(sample.x == 0 && sample.y == 0 && sample.scale == 1);
                if (motion == SendMotion::Rise)
                    assert(sample.x == 0 && sample.y > 0 && sample.y < 18 && sample.scale == 1);
                if (motion == SendMotion::Pop)
                    assert(sample.x == 0 && sample.y == 0 && sample.scale > .98f &&
                           sample.scale < 1);
                if (motion == SendMotion::Slide)
                    assert(sample.x == 12 && sample.y == 0 && sample.scale == 1);
                const auto opaque =
                    messageEntrance(duration / 2, false, 0, 0, 0, 0, true, SendMotion::None,
                                    SendReveal::None, motion, SendReveal::None, milliseconds);
                assert(sample.x == opaque.x && sample.y == opaque.y &&
                       sample.scale == opaque.scale);
                for (double age :
                     {-1., duration, duration + 1., std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::infinity()})
                    assertSettled(messageEntrance(age, false, 0, 0, 0, 0, true, SendMotion::None,
                                                  SendReveal::None, motion, reveal, milliseconds));
                assertSettled(messageEntrance(duration / 2, false, 0, 0, 0, 0, false,
                                              SendMotion::None, SendReveal::None, motion, reveal,
                                              milliseconds));
                // Incoming preferences never change outgoing motion or reveal.
                const auto outgoing =
                    messageEntrance(.2, true, 100, 500, 300, 100, true, SendMotion::Flight,
                                    SendReveal::Fade, motion, reveal, milliseconds);
                const auto legacy = messageEntrance(.2, true, 100, 500, 300, 100, true,
                                                    SendMotion::Flight, SendReveal::Fade);
                assert(outgoing.x == legacy.x && outgoing.y == legacy.y &&
                       outgoing.scale == legacy.scale && outgoing.reveal == legacy.reveal &&
                       outgoing.active == legacy.active);
            }
        }
    // Incoming Flight is never a composer flight, even for a malformed stored enum.
    assertSettled(messageEntrance(0, false, 100, 500, 300, 100, true, SendMotion::Flight,
                                  SendReveal::Fade, SendMotion::Flight, SendReveal::None));
    assert(messageIncomingDuration(-100) == .1 && messageIncomingDuration(0) == .1);
    assert(messageIncomingDuration(10000) == 2);
    for (float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()}) {
        assert(messageIncomingDuration(invalid) == incomingDuration);
        const auto received =
            messageEntrance(incomingDuration / 2, false, 0, 0, 0, 0, true, SendMotion::None,
                            SendReveal::None, SendMotion::Slide, SendReveal::Fade, invalid);
        assert(received.active && received.x == 12 && received.reveal == .5f);
        const auto flight = messageEntrance(.1, true, invalid, 0, 0, 0, true);
        assert(std::isfinite(flight.x) && std::isfinite(flight.y) && flight.x == 0 &&
               flight.y == 0);
    }
    const auto extreme = messageEntrance(0, true, std::numeric_limits<float>::max(), 0,
                                         -std::numeric_limits<float>::max(), 0, true);
    assert(std::isfinite(extreme.x) && std::isfinite(extreme.y));
#ifdef BLASTIRC_FX_INTEGRATION
    effectsIntegrationCheck();
#endif
}
