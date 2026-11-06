// Predictive Input Buffer (Geode 5 / GD 2.2081)
//
// CLICKS
//   If you press jump but the click can't do anything yet (cube in the air,
//   not touching an orb, ...), the click is held and fired on the FIRST tick
//   where it would actually work, as long as that tick is within N ticks of
//   your press (N = "Buffer Window", 2-16).
//     window = 2  -> 2 ticks early is saved, 3 ticks early still dies
//     window = 16 -> 16 ticks early is saved, 17+ still dies
//
// RELEASES
//   - If you release while your click is still waiting, the release is held
//     too and replayed after the click fires, with the same hold length.
//   - If a release lands in the same physics step as its own press, it is held
//     back one update so the press actually gets a step to register.
//
// Everything is tracked per player object, so dual mode / 2-player work too.
// Modes where a click always does something (ship, ufo, wave, swing) pass
// straight through untouched.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/loader/GameEvent.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>

using namespace geode::prelude;

namespace {
    // The click arrives in the same physics step as the first buffer check, but
    // that check sees the exact same player state the click did, so it can never
    // succeed. One extra tick makes "window = N" mean "N ticks early is saved".
    // If testing shows you're saved 1 tick too early/late, change this to 0 or 2.
    constexpr int kSameStepBias = 1;
    constexpr int kNever = -100;

    bool g_enabled = true;
    bool g_bufferReleases = true;
    int g_window = 4;
    bool g_dispatching = false; // true while WE feed an input to the game

    struct Slot {
        bool pending = false;   // a click is waiting for a valid tick
        bool physDown = false;  // is the physical button still held?
        int ticksLeft = 0;
        int heldTicks = 0;      // how long the player held it
        int pushCounter = kNever; // counter value when the last push was applied
        int releaseDue = -1;    // counter value at which to send a delayed release
    };

    // Would a click right now actually do something for this player?
    bool canAct(PlayerObject* p) {
        // ship / ufo / wave / swing: a click always matters, never buffer
        if (p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing) return true;
        // cube / ball / robot / spider: need a surface
        if (p->m_isOnGround || p->m_isOnSlope) return true;
        // orbs: need to be overlapping one
        if (p->m_touchingRings && p->m_touchingRings->count() > 0) return true;
        return false;
    }

    void toggleBuffer() {
        bool now = !g_enabled;
        g_enabled = now;
        Mod::get()->setSettingValue<bool>("buffer-enabled", now);
        Notification::create(
            now ? "Predictive Buffer: ON" : "Predictive Buffer: OFF",
            now ? NotificationIcon::Success : NotificationIcon::None,
            1.f
        )->show();
    }
}

$on_mod(Loaded) {
    g_enabled = Mod::get()->getSettingValue<bool>("buffer-enabled");
    g_bufferReleases = Mod::get()->getSettingValue<bool>("buffer-releases");
    g_window = static_cast<int>(std::clamp<int64_t>(
        Mod::get()->getSettingValue<int64_t>("buffer-ticks"), 2, 16));

    listenForSettingChanges("buffer-enabled", [](bool v) { g_enabled = v; });
    listenForSettingChanges("buffer-releases", [](bool v) { g_bufferReleases = v; });
    listenForSettingChanges("buffer-ticks", [](int64_t v) {
        g_window = static_cast<int>(std::clamp<int64_t>(v, 2, 16));
    });
}

$on_game(Loaded) {
    listenForKeybindSettingPresses("toggle-keybind",
        [](Keybind const& keybind, bool down, bool repeat, double timestamp) {
            if (down && !repeat) toggleBuffer();
        });
}

class $modify(PBPlayer, PlayerObject) {
    struct Fields {
        Slot s;
        int counter = 0; // physics updates seen by this player
    };

    // false for menu / garage / main-menu cubes
    bool pbInGame() {
        return this->isPlayer1() || this->isPlayer2();
    }

    // feed an input to the game without our own hooks catching it again
    void pbSend(bool down) {
        g_dispatching = true;
        if (down) this->pushButton(PlayerButton::Jump);
        else this->releaseButton(PlayerButton::Jump);
        g_dispatching = false;
    }

    bool pushButton(PlayerButton button) {
        if (button != PlayerButton::Jump || !g_enabled || g_dispatching || !pbInGame()) {
            return PlayerObject::pushButton(button);
        }

        auto& s = m_fields->s;
        int counter = m_fields->counter;

        // an old delayed release is still waiting: send it first so this
        // press is a clean press
        if (s.releaseDue >= 0) {
            s.releaseDue = -1;
            s.pushCounter = kNever;
            pbSend(false);
        }

        if (canAct(this)) {
            // click works right now -> vanilla behaviour
            s.pending = false;
            s.pushCounter = counter;
            return PlayerObject::pushButton(button);
        }

        // too early: hold the click and wait for a valid tick
        s.pending = true;
        s.physDown = true;
        s.heldTicks = 0;
        s.ticksLeft = g_window + kSameStepBias;
        return false;
    }

    bool releaseButton(PlayerButton button) {
        if (button != PlayerButton::Jump || !g_enabled || g_dispatching || !pbInGame()) {
            return PlayerObject::releaseButton(button);
        }

        auto& s = m_fields->s;

        // click still waiting: remember the release, replay it after the click
        if (s.pending) {
            s.physDown = false;
            return false;
        }

        // release in the same step as its own press: give the press one update
        if (g_bufferReleases && s.pushCounter != kNever && m_fields->counter == s.pushCounter) {
            s.releaseDue = s.pushCounter + 2;
            return false;
        }

        s.pushCounter = kNever;
        return PlayerObject::releaseButton(button);
    }

    // runs once per physics update, right before the player moves, so an input
    // we fire here takes effect on that very step
    void update(float dt) {
        auto& s = m_fields->s;
        int& counter = m_fields->counter;
        ++counter;

        // delayed release
        if (s.releaseDue >= 0 && counter >= s.releaseDue) {
            s.releaseDue = -1;
            s.pushCounter = kNever;
            pbSend(false);
        }

        // buffered click
        if (s.pending) {
            if (!g_enabled || m_isDead) {
                s.pending = false;
            } else {
                if (s.physDown) ++s.heldTicks;

                if (canAct(this)) {
                    // first valid tick: fire the buffered click
                    s.pending = false;
                    s.pushCounter = counter - 1; // this update will be the first to see it
                    pbSend(true);
                    if (!s.physDown) {
                        // player already let go: replay the hold length
                        int hold = g_bufferReleases ? std::max(1, s.heldTicks) : 1;
                        s.releaseDue = counter + hold;
                    }
                } else if (--s.ticksLeft <= 0) {
                    // window ran out: the click was too early and is dropped
                    s.pending = false;
                }
            }
        }

        PlayerObject::update(dt);
    }

    // don't carry buffered inputs across a death / restart
    void resetObject() {
        m_fields->s = Slot{};
        PlayerObject::resetObject();
    }
};
