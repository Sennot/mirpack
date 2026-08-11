#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>

#include "overlay.hpp"

using namespace geode::prelude;

namespace cleanfeed {
    namespace {
        void toggleSetting(char const* key, char const* label) {
            if (!overlay::layer()) return;

            auto* mod = Mod::get();
            auto const enabled = !mod->getSettingValue<bool>(key);
            mod->setSettingValue<bool>(key, enabled);

            Notification::create(
                fmt::format("{}: {}", label, enabled ? "ON" : "OFF"),
                enabled ? NotificationIcon::Success : NotificationIcon::Info,
                0.8f
            )->show();
        }
    }
}

$on_game(Loaded) {
    listenForKeybindSettingPresses(
        "toggle-hitboxes-keybind",
        [](Keybind const&, bool down, bool repeat, double) {
            if (down && !repeat) cleanfeed::toggleSetting("show-hitboxes", "Hitboxes");
        }
    );

    listenForKeybindSettingPresses(
        "toggle-trajectory-keybind",
        [](Keybind const&, bool down, bool repeat, double) {
            if (down && !repeat) cleanfeed::toggleSetting("show-trajectory", "Trajectory");
        }
    );
}
