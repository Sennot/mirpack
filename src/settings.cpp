#include "settings.hpp"

#include <algorithm>

using namespace geode::prelude;

namespace cleanfeed::settings {
    bool enabled() {
        return Mod::get()->getSettingValue<bool>("enabled");
    }

    std::string senderName() {
        auto value = Mod::get()->getSettingValue<std::string>("sender-name");
        return value.empty() ? "Geometry Dash Clean Feed" : value;
    }

    bool showHitboxes() {
        return Mod::get()->getSettingValue<bool>("show-hitboxes");
    }

    bool showTrajectory() {
        return Mod::get()->getSettingValue<bool>("show-trajectory");
    }

    int trajectoryTps() {
        return std::clamp(static_cast<int>(Mod::get()->getSettingValue<int64_t>("trajectory-tps")), 60, 1000);
    }

    float trajectoryLength() {
        return std::clamp(static_cast<float>(Mod::get()->getSettingValue<double>("trajectory-length")), 0.1f, 5.f);
    }

    float trajectoryWidth() {
        return std::clamp(static_cast<float>(Mod::get()->getSettingValue<double>("trajectory-width")), 0.1f, 5.f);
    }

    float hitboxWidth() {
        return std::clamp(static_cast<float>(Mod::get()->getSettingValue<double>("hitbox-width")), 0.05f, 5.f);
    }

    float hitboxFillOpacity() {
        return std::clamp(static_cast<float>(Mod::get()->getSettingValue<double>("hitbox-fill-opacity")), 0.f, 1.f);
    }

    cocos2d::ccColor4F color(std::string_view key) {
        auto const value = Mod::get()->getSettingValue<cocos2d::ccColor4B>(key);
        constexpr float scale = 1.f / 255.f;
        return {
            value.r * scale,
            value.g * scale,
            value.b * scale,
            value.a * scale,
        };
    }

    cocos2d::ccColor4F colorWithAlpha(std::string_view key, float alpha) {
        auto value = color(key);
        value.a *= std::clamp(alpha, 0.f, 1.f);
        return value;
    }
}
