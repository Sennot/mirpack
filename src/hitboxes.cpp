// Hitbox categories and visible-section traversal were informed by Eclipse Menu
// (EPL-2.0); this implementation is purpose-built for the shared overlay root.
#include "hitboxes.hpp"

#include "overlay.hpp"
#include "settings.hpp"

#include <algorithm>
#include <array>
#include <chrono>

using namespace geode::prelude;

namespace cleanfeed::hitboxes {
    namespace {
        using Clock = std::chrono::steady_clock;

        cocos2d::CCDrawNode* s_objectNode = nullptr;
        cocos2d::CCDrawNode* s_playerNode = nullptr;
        GJBaseGameLayer* s_layer = nullptr;
        Clock::time_point s_nextObjectRefresh{};
        bool s_objectsDirty = true;
        bool s_wasEnabled = false;

        struct Palette {
            cocos2d::ccColor4F solid;
            cocos2d::ccColor4F hazard;
            cocos2d::ccColor4F interactable;
            cocos2d::ccColor4F solidFill;
            cocos2d::ccColor4F hazardFill;
            cocos2d::ccColor4F interactableFill;
        };

        void drawRect(
            cocos2d::CCDrawNode* node,
            cocos2d::CCRect const& rect,
            cocos2d::ccColor4F const& fill,
            float width,
            cocos2d::ccColor4F const& border
        ) {
            std::array<cocos2d::CCPoint, 4> vertices = {
                cocos2d::CCPoint{rect.getMinX(), rect.getMinY()},
                cocos2d::CCPoint{rect.getMinX(), rect.getMaxY()},
                cocos2d::CCPoint{rect.getMaxX(), rect.getMaxY()},
                cocos2d::CCPoint{rect.getMaxX(), rect.getMinY()},
            };
            node->drawPolygon(
                vertices.data(), static_cast<unsigned int>(vertices.size()), fill, width, border
            );
        }

        void drawObject(GJBaseGameLayer* layer, GameObject* object, float width, Palette const& palette) {
            if (!object || object->m_objectType == GameObjectType::Decoration ||
                !object->m_isActivated || object->m_isGroupDisabled) {
                return;
            }

            switch (object->m_objectType) {
                case GameObjectType::CollisionObject:
                case GameObjectType::Decoration:
                    return;

                case GameObjectType::Solid: {
                    drawRect(s_objectNode, object->getObjectRect(), palette.solidFill, width, palette.solid);
                    return;
                }

                case GameObjectType::Slope: {
                    auto const rect = object->getObjectRect();
                    std::array<cocos2d::CCPoint, 3> vertices = {
                        cocos2d::CCPoint{rect.getMinX(), rect.getMinY()},
                        cocos2d::CCPoint{rect.getMinX(), rect.getMaxY()},
                        cocos2d::CCPoint{rect.getMaxX(), rect.getMinY()},
                    };
                    auto const topRight = cocos2d::CCPoint{rect.getMaxX(), rect.getMaxY()};
                    switch (object->m_slopeDirection) {
                        case 0:
                        case 7: vertices[1] = topRight; break;
                        case 1:
                        case 5: vertices[0] = topRight; break;
                        case 3:
                        case 6: vertices[2] = topRight; break;
                        default: break;
                    }
                    s_objectNode->drawPolygon(
                        vertices.data(), static_cast<unsigned int>(vertices.size()),
                        palette.solidFill, width, palette.solid
                    );
                    return;
                }

                case GameObjectType::AnimatedHazard:
                case GameObjectType::Hazard: {
                    if (object == layer->m_anticheatSpike) return;
                    auto const radius = std::max(object->m_scaleX, object->m_scaleY) * object->m_objectRadius;
                    if (radius > 0.f) {
                        s_objectNode->drawCircle(
                            object->getPosition(), radius, palette.hazardFill,
                            width, palette.hazard, 12
                        );
                    } else if (auto* oriented = layer->m_isEditor ? object->getOrientedBox() : object->m_orientedBox) {
                        s_objectNode->drawPolygon(
                            oriented->m_corners.data(), 4,
                            palette.hazardFill, width, palette.hazard
                        );
                    } else {
                        auto const rectDirty = object->m_isObjectRectDirty;
                        auto const offsetCalculated = object->m_boxOffsetCalculated;
                        drawRect(
                            s_objectNode, object->getObjectRect(),
                            palette.hazardFill, width, palette.hazard
                        );
                        object->m_isObjectRectDirty = rectDirty;
                        object->m_boxOffsetCalculated = offsetCalculated;
                    }
                    return;
                }

                default: {
                    if (object == layer->m_player1 || object == layer->m_player2) return;
                    if (object->m_objectType == GameObjectType::Modifier &&
                        !static_cast<EffectGameObject*>(object)->m_isTouchTriggered) {
                        return;
                    }
                    if (auto* oriented = layer->m_isEditor ? object->getOrientedBox() : object->m_orientedBox) {
                        s_objectNode->drawPolygon(
                            oriented->m_corners.data(), 4,
                            palette.interactableFill, width, palette.interactable
                        );
                    } else {
                        auto const rectDirty = object->m_isObjectRectDirty;
                        auto const offsetCalculated = object->m_boxOffsetCalculated;
                        drawRect(
                            s_objectNode, object->getObjectRect(),
                            palette.interactableFill, width, palette.interactable
                        );
                        object->m_isObjectRectDirty = rectDirty;
                        object->m_boxOffsetCalculated = offsetCalculated;
                    }
                    return;
                }
            }
        }

        template <class Callback>
        void forEachVisibleObject(GJBaseGameLayer* layer, Callback&& callback) {
            auto const columnCount = static_cast<int>(layer->m_sections.size());
            if (columnCount <= 0) return;

            auto const firstColumn = std::max(0, layer->m_leftSectionIndex);
            auto const lastColumn = std::min(columnCount - 1, layer->m_rightSectionIndex);
            for (int x = firstColumn; x <= lastColumn; ++x) {
                auto* column = layer->m_sections[x];
                if (!column || x >= static_cast<int>(layer->m_sectionSizes.size())) continue;
                auto* sizes = layer->m_sectionSizes[x];
                if (!sizes) continue;

                auto const rowCount = static_cast<int>(column->size());
                auto const firstRow = std::max(0, layer->m_bottomSectionIndex);
                auto const lastRow = std::min(rowCount - 1, layer->m_topSectionIndex);
                for (int y = firstRow; y <= lastRow; ++y) {
                    auto* section = column->at(y);
                    if (!section || y >= static_cast<int>(sizes->size())) continue;
                    auto const count = std::min(static_cast<int>(section->size()), sizes->at(y));
                    for (int index = 0; index < count; ++index) {
                        callback(section->at(index));
                    }
                }
            }
        }

        void drawPlayer(PlayerObject* player, float width, float fillAlpha) {
            if (!player) return;

            auto const playerColor = settings::color("player-color");
            auto const innerColor = settings::color("player-inner-color");
            auto const rotatedColor = settings::color("player-rotated-color");
            auto const playerFill = settings::colorWithAlpha("player-color", fillAlpha);
            auto const innerFill = settings::colorWithAlpha("player-inner-color", fillAlpha);
            auto const rotatedFill = settings::colorWithAlpha("player-rotated-color", fillAlpha);

            if (auto* oriented = player->m_orientedBox) {
                s_playerNode->drawPolygon(
                    oriented->m_corners.data(), 4, rotatedFill, width, rotatedColor
                );
            }
            drawRect(s_playerNode, player->getObjectRect(), playerFill, width, playerColor);
            drawRect(
                s_playerNode, player->getObjectRect(0.3f, 0.3f),
                innerFill, width, innerColor
            );
        }
    }

    void attach(GJBaseGameLayer* layer) {
        auto* root = overlay::root();
        if (!layer || !root) return;

        s_layer = layer;
        s_objectNode = cocos2d::CCDrawNode::create();
        s_objectNode->m_bUseArea = false;
        s_objectNode->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        s_objectNode->setID("object-hitboxes"_spr);
        root->addChild(s_objectNode, 10);

        s_playerNode = cocos2d::CCDrawNode::create();
        s_playerNode->m_bUseArea = false;
        s_playerNode->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        s_playerNode->setID("player-hitboxes"_spr);
        root->addChild(s_playerNode, 11);

        s_nextObjectRefresh = {};
        s_objectsDirty = true;
        s_wasEnabled = false;
    }

    void detach(GJBaseGameLayer* layer) {
        if (layer != s_layer) return;
        s_objectNode = nullptr;
        s_playerNode = nullptr;
        s_layer = nullptr;
        s_nextObjectRefresh = {};
        s_objectsDirty = true;
        s_wasEnabled = false;
    }

    void update(GJBaseGameLayer* layer) {
        if (!s_objectNode || !s_playerNode || layer != s_layer) return;

        auto const enabled = settings::showHitboxes();
        if (!enabled) {
            if (s_wasEnabled) {
                s_objectNode->clear();
                s_playerNode->clear();
            }
            s_wasEnabled = false;
            s_objectsDirty = true;
            return;
        }

        if (!s_wasEnabled) s_objectsDirty = true;
        s_wasEnabled = true;

        auto const zoom = std::max(0.01f, layer->m_gameState.m_cameraZoom);
        auto const width = settings::hitboxWidth() / zoom;
        auto const fillAlpha = settings::hitboxFillOpacity();
        Palette const palette = {
            .solid = settings::color("solid-color"),
            .hazard = settings::color("hazard-color"),
            .interactable = settings::color("interactable-color"),
            .solidFill = settings::colorWithAlpha("solid-color", fillAlpha),
            .hazardFill = settings::colorWithAlpha("hazard-color", fillAlpha),
            .interactableFill = settings::colorWithAlpha("interactable-color", fillAlpha),
        };

        // Environment geometry is the expensive part on object-heavy levels.
        // Refresh it at 30 Hz while keeping player hitboxes at the render rate.
        auto const now = Clock::now();
        if (s_objectsDirty || now >= s_nextObjectRefresh) {
            auto const started = now;
            s_objectNode->clear();
            forEachVisibleObject(layer, [&](GameObject* object) {
                drawObject(layer, object, width, palette);
            });

            auto const cost = Clock::now() - started;
            auto const basePeriod = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / 30.0)
            );
            auto const adaptivePeriod = std::max(basePeriod, cost * 3);
            s_nextObjectRefresh = started + adaptivePeriod;
            s_objectsDirty = false;
        }

        s_playerNode->clear();
        drawPlayer(layer->m_player1, width, fillAlpha);
        if (layer->m_gameState.m_isDualMode) drawPlayer(layer->m_player2, width, fillAlpha);
    }
}
