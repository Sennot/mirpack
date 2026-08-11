#pragma once

#include <Geode/Geode.hpp>

namespace cleanfeed::settings {
    bool enabled();
    std::string senderName();
    bool showHitboxes();
    bool showTrajectory();
    int trajectoryTps();
    float trajectoryLength();
    float trajectoryWidth();
    float hitboxWidth();
    float hitboxFillOpacity();

    cocos2d::ccColor4F color(std::string_view key);
    cocos2d::ccColor4F colorWithAlpha(std::string_view key, float alpha);
}
