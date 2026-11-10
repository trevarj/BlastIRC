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
        const auto expectClip = [&](std::string_view name) {
            assert(playing() == 1);
            for (const auto &voice : p.voices)
                if (MIX_TrackPlaying(voice.track)) {
                    std::size_t clip = 0;
                    while (clip < std::size(clipNames) && name != clipNames[clip])
                        ++clip;
                    assert(clip < p.audio.size());
                    assert(MIX_GetTrackAudio(voice.track) == p.audio[clip]);
                    assert(MIX_GetTrackGain(voice.track) > 0);
                }
        };
        // Observe the real visual simulation consumed by the overlay, without a GL context.
        std::vector<std::vector<float>> normal;
        for (bool silent : {false, true}) {
            p.stop();
            assert(playing() == 0);
            Event event;
            event.type = "mention";
            event.server = "synthetic";
            event.target = "#chat";
            event.id = "row";
            event.silent = silent;
            const double start = silent ? 20 : 10;
            model.effects.clear();
            model.now = start;
            effects.draw(model); // Equal zero-age first frames through the public entry.
            model.effects = {event};
            for (int frame = 0; frame <= 70; ++frame) {
                model.now = start + frame * .05;
                fxViewport(0, 0, 128, 80);
                // Moving the real row during the bloom must move both veils identically.
                fxAnchor("synthetic", "#chat", "row", frame < 10 ? 12 : 24, 30, 60, 12);
                effects.draw(model);
                model.effects.clear();
                if (frame == 0) {
                    if (silent)
                        assert(playing() == 0);
                    else
                        expectClip("ding");
                }
                std::vector<float> geometry;
                if (frame <= 65)
                    assert(p.count == 1);
                if (p.count) {
                    assert(p.count == 1);
                    const auto &particle = p.particles[0];
                    const float x = frame < 10 ? 12 : 24;
                    assert(particle.shape == Shape::Veil && particle.life == 3.3f);
                    assert(std::abs(particle.age - frame * .05f) < .00001f);
                    assert(particle.rect.min.x == x && particle.rect.min.y == 30);
                    assert(particle.rect.max.x == x + 60 && particle.rect.max.y == 42);
                    const auto renderedCenter = center(particle.rect);
                    assert(renderedCenter.x == x + 30 && renderedCenter.y == 36);
                    assert(particle.clip.min.x == 0 && particle.clip.min.y == 0);
                    assert(particle.clip.max.x == 128 && particle.clip.max.y == 80);
                    geometry = {particle.age,        particle.life,       particle.rect.min.x,
                                particle.rect.min.y, particle.rect.max.x, particle.rect.max.y,
                                particle.pos.x,      particle.pos.y};
                }
                if (silent)
                    assert(geometry == normal[frame]);
                else
                    normal.push_back(geometry);
                if (frame == 70)
                    assert(p.count == 0); // Real expiry, not a permanently retained visual.
            }
        }
        assert(normal[6][2] + 12 == normal[26][2]);
        assert(normal[6][6] + 12 == normal[26][6]);
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
        assert(veil(t * 4) >= 0 && veil(t * 4) <= 1);
    }
    assert(veil(0) == 0 && veil(.3f) == 1 && veil(1.3f) == 1 && veil(3.3f) == 0);
    // Gentle attention has one continuous bloom and one continuous decay, no strobe.
    for (int i = 1; i <= 300; ++i)
        assert(veil(i * .001f) + .00001f >= veil((i - 1) * .001f));
    for (int i = 1301; i <= 3300; ++i)
        assert(veil(i * .001f) <= veil((i - 1) * .001f) + .00001f);
    // One slow, continuous composer-to-slot flight; interruption settles immediately.
    const auto start = messageEntrance(0, true, 100, 500, 300, 100, true);
    const auto middle = messageEntrance(outgoingDuration / 2, true, 100, 500, 300, 100, true);
    assert(start.active && start.x == -200 && start.y == 400 && start.reveal == 0);
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
    const auto incoming = messageEntrance(incomingDuration / 2, false, 100, 500, 300, 100, true);
    assert(incoming.active && incoming.x == 0 && incoming.y == 0 && incoming.reveal > 0 &&
           incoming.reveal < 1);
    // Reveal can finish before flight, and motion before reveal, without an early retirement.
    const auto flying =
        messageEntrance(.5, true, 100, 500, 300, 100, true, SendMotion::Flight, SendReveal::Fade);
    assert(flying.active && flying.reveal == 1 && flying.y > 0);
    const auto revealing =
        messageEntrance(.4, true, 100, 500, 300, 100, true, SendMotion::Pop, SendReveal::Pixels);
    assert(revealing.active && revealing.scale == 1 && revealing.reveal < 1);
    for (auto motion : {SendMotion::None, SendMotion::Flight, SendMotion::Rise, SendMotion::Pop,
                        SendMotion::Slide, SendMotion::Drift})
        for (auto reveal : {SendReveal::None, SendReveal::Fade, SendReveal::Pixels}) {
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
            if (motion == SendMotion::Drift)
                assert(sample.x > 0 && sample.x < 18 && sample.y > 0 && sample.y < 12);
            const auto settled =
                messageEntrance(lifetime, true, 100, 500, 300, 100, true, motion, reveal);
            assert(!settled.active && settled.x == 0 && settled.y == 0 && settled.scale == 1 &&
                   settled.reveal == 1);
            const auto disabled =
                messageEntrance(.1, true, 100, 500, 300, 100, false, motion, reveal);
            assert(!disabled.active && disabled.scale == 1 && disabled.reveal == 1);
            const auto received =
                messageEntrance(.1, false, 100, 500, 300, 100, true, motion, reveal);
            assert(received.active && received.x == 0 && received.y == 0 && received.scale == 1 &&
                   received.reveal == float(.1 / incomingDuration));
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
    for (auto motion : {SendMotion::None, SendMotion::Rise, SendMotion::Pop, SendMotion::Slide,
                        SendMotion::Drift})
        for (auto reveal : {SendReveal::None, SendReveal::Fade, SendReveal::Pixels}) {
            for (float milliseconds : {100.f, 550.f, 2000.f}) {
                const double duration = messageIncomingDuration(milliseconds);
                const auto sample =
                    messageEntrance(duration / 2, false, 0, 0, 0, 0, true, SendMotion::Flight,
                                    SendReveal::Pixels, motion, reveal, milliseconds);
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
                    assert(sample.x == 0 && sample.y == 0 && sample.scale > 1 &&
                           sample.scale < 1.02f);
                if (motion == SendMotion::Slide)
                    assert(sample.x == 12 && sample.y == 0 && sample.scale == 1);
                if (motion == SendMotion::Drift)
                    assert(sample.x == 9 && sample.y > 0 && sample.y < 12);
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
                                  SendReveal::Pixels, SendMotion::Flight, SendReveal::None));
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
    // Block reveal is monotonic, spatially staggered and fully sharp at completion.
    bool staggered = false;
    for (unsigned block = 0; block < 64; ++block) {
        assert(messageBlockOpacity(0, block) == 0 && messageBlockOpacity(1, block) == 1);
        for (int step = 1; step <= 100; ++step)
            assert(messageBlockOpacity(step / 100.f, block) >=
                   messageBlockOpacity((step - 1) / 100.f, block));
        staggered |= messageBlockOpacity(.5f, block) != messageBlockOpacity(.5f, 0);
    }
    assert(staggered);
#ifdef BLASTIRC_FX_INTEGRATION
    effectsIntegrationCheck();
#endif
}
