// Physics/prediction behavior adapted from Silicate commit f183dbc5
// (GPL-3.0). Bot, replay, renderer and UI coupling were removed.
#include "trajectory.hpp"

#include "checkpoint/checkpoint.hpp"
#include "overlay.hpp"
#include "physics/collisions.hpp"
#include "physics/object.hpp"
#include "settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>

using namespace geode::prelude;

namespace cleanfeed {
    namespace {
        uint32_t packColor(cocos2d::ccColor4F const& color) {
            auto const channel = [](float value) {
                return static_cast<uint32_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
            };
            return channel(color.r) | (channel(color.g) << 8u) |
                   (channel(color.b) << 16u) | (channel(color.a) << 24u);
        }

        uint64_t packPlayerFlags(PlayerObject* player) {
            if (!player) return 0;
            uint64_t flags = 0;
            unsigned int bit = 0;
            auto set = [&](bool value) {
                if (value) flags |= uint64_t{1} << bit;
                ++bit;
            };
            set(player->m_isShip);
            set(player->m_isBird);
            set(player->m_isDart);
            set(player->m_isSwing);
            set(player->m_isBall);
            set(player->m_isSpider);
            set(player->m_isRobot);
            set(player->m_isUpsideDown);
            set(player->m_isDead);
            set(player->m_isOnGround2);
            set(player->m_isSideways);
            set(player->m_isDashing);
            set(player->m_isAccelerating);
            set(player->m_holdingLeft);
            set(player->m_holdingRight);
            set(player->m_jumpBuffered);
            set(player->m_isPlatformer);
            set(player->m_isGoingLeft);
            set(player->m_maybeIsBoosted);
            return flags;
        }

        cocos2d::CCRect shrink(cocos2d::CCRect rect, float amount) {
            rect.origin.x += amount;
            rect.origin.y += amount;
            rect.size.width -= amount * 2.f;
            rect.size.height -= amount * 2.f;
            return rect;
        }

        void drawRotatedRect(
            cocos2d::CCDrawNode* node,
            cocos2d::CCRect const& rect,
            float angle,
            cocos2d::ccColor4F const& color,
            float width
        ) {
            std::array<cocos2d::CCPoint, 4> vertices = {
                cocos2d::CCPoint{rect.getMinX(), rect.getMinY()},
                cocos2d::CCPoint{rect.getMaxX(), rect.getMinY()},
                cocos2d::CCPoint{rect.getMaxX(), rect.getMaxY()},
                cocos2d::CCPoint{rect.getMinX(), rect.getMaxY()},
            };
            for (auto& vertex : vertices) {
                vertex = vertex.rotateByAngle(
                    {rect.getMidX(), rect.getMidY()},
                    -CC_DEGREES_TO_RADIANS(angle)
                );
            }
            node->drawPolygon(
                vertices.data(), static_cast<unsigned int>(vertices.size()),
                {0.f, 0.f, 0.f, 0.f}, width, color
            );
        }
    }

    TrajectoryDrawNode* TrajectoryDrawNode::create() {
        auto* result = new TrajectoryDrawNode();
        if (result && result->init()) {
            result->autorelease();
            result->m_bUseArea = false;
            return result;
        }
        CC_SAFE_DELETE(result);
        return nullptr;
    }

    Trajectory& Trajectory::get() {
        static Trajectory instance;
        return instance;
    }

    PlayerObject* Trajectory::createFakePlayer(GJBaseGameLayer* layer, std::string const& id) {
        auto* player = PlayerObject::create(1, 1, layer, layer, true);
        if (!player) return nullptr;

        // copyAttributes aliases engine-managed PlayerObject internals. The
        // original Silicate implementation deliberately retains simulation
        // players for the process lifetime so scene teardown never destroys an
        // aliased object. Releasing one here crashes in PlayerObject::~PlayerObject.
        player->retain();
        player->setPosition({0.f, 105.f});
        player->setVisible(false);
        player->setID(id);
        layer->m_objectLayer->addChild(player);
        return player;
    }

    void Trajectory::init(GJBaseGameLayer* layer) {
        shutdown(m_layer);
        if (!layer || !overlay::root()) return;

        m_layer = layer;
        m_node = TrajectoryDrawNode::create();
        m_node->setID("trajectory"_spr);
        m_node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        overlay::root()->addChild(m_node, 20);

        m_fakePlayer1 = createFakePlayer(layer, "trajectory-fake-player-1"_spr);
        m_fakePlayer2 = createFakePlayer(layer, "trajectory-fake-player-2"_spr);
        m_calculated = false;
        m_p1Holding = false;
        m_p2Holding = false;
    }

    void Trajectory::shutdown(GJBaseGameLayer* layer) {
        if (m_layer && layer && layer != m_layer) return;

        if (m_fakePlayer1 && m_fakePlayer1->getParent()) m_fakePlayer1->removeFromParent();
        if (m_fakePlayer2 && m_fakePlayer2->getParent()) m_fakePlayer2->removeFromParent();
        if (m_node && m_node->getParent()) m_node->removeFromParent();

        m_layer = nullptr;
        m_node = nullptr;
        m_fakePlayer1 = nullptr;
        m_fakePlayer2 = nullptr;
        m_actions.clear();
        clearActivatedObjects();
        m_calculated = false;
        m_drawing = false;
    }

    bool Trajectory::drawing() const {
        return m_drawing;
    }

    bool Trajectory::isFakePlayer(PlayerObject* player) const {
        return player && (player == m_fakePlayer1 || player == m_fakePlayer2);
    }

    PlayerObject* Trajectory::getOtherPlayer(PlayerObject* player) const {
        return player == m_fakePlayer1 ? m_fakePlayer2 : m_fakePlayer1;
    }

    bool Trajectory::hasDied(PlayerObject* player) {
        if (player == m_fakePlayer1) {
            m_deadP1 = true;
            return true;
        }
        if (player == m_fakePlayer2) {
            m_deadP2 = true;
            return true;
        }
        return false;
    }

    void Trajectory::handleButton(bool player1, bool holding) {
        if (player1) m_p1Holding = holding;
        else m_p2Holding = holding;
        m_calculated = false;
    }

    void Trajectory::rememberActivatedObject(EnhancedGameObject* object, PlayerObject* player) {
        if (!object) return;
        if (player == m_fakePlayer1) m_activatedObjectsP1.insert(reinterpret_cast<uintptr_t>(object));
        else if (player == m_fakePlayer2) m_activatedObjectsP2.insert(reinterpret_cast<uintptr_t>(object));
    }

    bool Trajectory::playerHasActivated(PlayerObject* player, EnhancedGameObject* object) const {
        if (!object || object->m_isMultiActivate) return false;
        auto const address = reinterpret_cast<uintptr_t>(object);
        if (player == m_fakePlayer1) return m_activatedObjectsP1.contains(address);
        if (player == m_fakePlayer2) return m_activatedObjectsP2.contains(address);
        return false;
    }

    bool Trajectory::realPlayerHasActivated(PlayerObject* player, EnhancedGameObject* object) const {
        if (!object || !m_layer) return false;
        if (!isFakePlayer(player)) return phys::hasBeenActivatedByPlayer(player, object);
        auto* realPlayer = player == m_fakePlayer1 ? m_layer->m_player1 : m_layer->m_player2;
        return realPlayer && phys::hasBeenActivatedByPlayer(realPlayer, object);
    }

    void Trajectory::clearActivatedObjects() {
        m_activatedObjectsP1.clear();
        m_activatedObjectsP2.clear();
    }

    Trajectory::Signature Trajectory::computeSignature(GJBaseGameLayer* layer) const {
        Signature signature;
        auto const storePlayer = [](PlayerObject* player, float (&values)[7], uint64_t& flags) {
            if (!player) return;
            auto const position = player->getPosition();
            values[0] = position.x;
            values[1] = position.y;
            values[2] = player->getRotation();
            values[3] = static_cast<float>(player->m_yVelocity);
            values[4] = static_cast<float>(player->m_platformerXVelocity);
            values[5] = player->m_gravityMod;
            values[6] = player->m_vehicleSize;
            flags = packPlayerFlags(player);
        };
        storePlayer(layer->m_player1, signature.p1, signature.p1Flags);
        storePlayer(layer->m_player2, signature.p2, signature.p2Flags);

        signature.timeWarp = layer->m_gameState.m_timeWarp;
        signature.cameraZoom = layer->m_gameState.m_cameraZoom;
        signature.lineWidth = settings::trajectoryWidth();
        signature.length = settings::trajectoryLength();
        signature.tps = settings::trajectoryTps();
        signature.colors[0] = packColor(settings::color("trajectory-hold-color"));
        signature.colors[1] = packColor(settings::color("trajectory-release-color"));

        uint32_t flags = 0;
        flags |= settings::showTrajectory() ? 1u : 0u;
        flags |= layer->m_gameState.m_isDualMode ? 2u : 0u;
        flags |= layer->m_isPlatformer ? 4u : 0u;
        flags |= layer->m_levelSettings->m_twoPlayerMode ? 8u : 0u;
        flags |= m_p1Holding ? 16u : 0u;
        flags |= m_p2Holding ? 32u : 0u;
        signature.boolPack = flags;
        return signature;
    }

    bool Trajectory::iterate(
        GJBaseGameLayer* layer,
        PlayerObject* player,
        int mode,
        cocos2d::ccColor4F const& color,
        int& stepCount
    ) {
        auto const previousPosition = player->getPosition();

        layer->m_gameState.m_totalTime += m_physicsDt;
        auto const timeWarp = std::max(0.001f, layer->m_gameState.m_timeWarp);
        layer->m_gameState.m_unkDouble3 += m_physicsDt / timeWarp;
        ++layer->m_gameState.m_currentProgress;
        layer->m_gameState.m_unkUint5 += static_cast<int>(std::round(timeWarp * 1000.f));
        player->m_totalTime += m_physicsDt;

        auto const playerSpeed = *reinterpret_cast<float*>(&layer->m_gameState.m_timeModRelated);
        if (playerSpeed != 0.f) {
            layer->m_gameState.m_timeModRelated = 0;
            layer->m_gameState.m_timeModRelated2 = 0;
            player->updateTimeMod(playerSpeed, true);
        }

        player->m_collisionLogTop->removeAllObjects();
        player->m_collisionLogBottom->removeAllObjects();
        player->m_collisionLogLeft->removeAllObjects();
        player->m_collisionLogRight->removeAllObjects();

        auto const dead = ((mode & Player1) && m_deadP1) || ((mode & Player2) && m_deadP2);
        if (dead) {
            if (stepCount > 1) drawHitbox(player);
            return true;
        }

        for (auto& action : m_actions) {
            if (action.delay == 0) {
                action.function();
                action.executed = true;
            } else {
                --action.delay;
            }
        }
        std::erase_if(m_actions, [](Action const& action) { return action.executed; });

        player->m_playEffects = false;
        player->update(m_playerDelta);
        player->m_unkUnused3 = player->getRotation();
        player->updateRotation(m_playerDelta);
        player->m_shipRotation = player->getPosition();

        if (layer->checkCollisions(player, m_playerDelta, false) == 1) hasDied(player);
        phys::checkSpawnObjects(layer, player);
        layer->m_effectManager->postCollisionCheck();

        auto const zoom = std::max(0.01f, layer->m_gameState.m_cameraZoom);
        m_node->drawSegment(previousPosition, player->getPosition(), settings::trajectoryWidth() / zoom, color);
        ++stepCount;
        return false;
    }

    void Trajectory::runPrediction(
        GJBaseGameLayer* layer,
        PlayerObject* player,
        PlayerObject* other,
        int mode,
        bool both
    ) {
        std::array<PlayerObject*, 2> players = {player, other};
        auto const count = both && layer->m_gameState.m_isDualMode ? 2 : 1;
        for (int index = 0; index < count; ++index) {
            auto* current = players[index];
            switch (mode & (Hold | Swift | Release)) {
                case Hold: current->pushButton(PlayerButton::Jump); break;
                case Swift:
                    current->pushButton(PlayerButton::Jump);
                    current->releaseButton(PlayerButton::Jump);
                    break;
                case Release:
                    current->releaseButton(PlayerButton::Jump);
                    current->m_jumpBuffered = false;
                    break;
                default: break;
            }

            if (mode & Left) {
                current->releaseButton(PlayerButton::Right);
                current->pushButton(PlayerButton::Left);
            } else if (mode & Right) {
                current->releaseButton(PlayerButton::Left);
                current->pushButton(PlayerButton::Right);
            }
        }

        auto const iterations = static_cast<int>(
            settings::trajectoryLength() * static_cast<float>(settings::trajectoryTps()) /
            std::max(0.001f, layer->m_gameState.m_timeWarp)
        );
        auto const color = (mode & Hold)
            ? settings::color("trajectory-hold-color")
            : settings::color("trajectory-release-color");
        auto const playerMask = player == m_fakePlayer1 ? Player1 : Player2;
        int playerSteps = 0;
        int otherSteps = 0;
        bool playerDone = false;
        bool otherDone = false;

        for (int index = 0; index < iterations && (!playerDone || (both && !otherDone)); ++index) {
            if (!playerDone) playerDone = iterate(layer, player, mode | playerMask, color, playerSteps);
            if (both && layer->m_gameState.m_isDualMode && !otherDone) {
                auto inverse = color;
                inverse.r = 1.f - inverse.r;
                inverse.g = 1.f - inverse.g;
                inverse.b = 1.f - inverse.b;
                otherDone = iterate(layer, other, mode | Player2, inverse, otherSteps);
            }
        }
    }

    void Trajectory::simulate(GJBaseGameLayer* layer, bool player1, int mode, bool clickBothPlayers) {
        auto* player = player1 ? m_fakePlayer1 : m_fakePlayer2;
        auto* realPlayer = player1 ? layer->m_player1 : layer->m_player2;
        auto* other = player1 ? m_fakePlayer2 : m_fakePlayer1;
        auto* otherReal = player1 ? layer->m_player2 : layer->m_player1;
        if (!player || !realPlayer || !other) return;

        GJGameState gameState = layer->m_gameState;
        EffectManagerState effectState;
        layer->m_effectManager->saveToState(effectState);

        player->copyAttributes(realPlayer);
        player->m_maybeReducedEffects = true;
        auto checkpoint = SavedPlayerCheckpoint::create(realPlayer);
        checkpoint.apply(player);
        player->setPosition(realPlayer->m_position);
        player->setRotation(realPlayer->getRotation());

        if (clickBothPlayers && otherReal) {
            other->copyAttributes(otherReal);
            other->m_maybeReducedEffects = true;
            auto otherCheckpoint = SavedPlayerCheckpoint::create(otherReal);
            otherCheckpoint.apply(other);
            other->setPosition(otherReal->m_position);
            other->setRotation(otherReal->getRotation());
        }

        m_deadP1 = false;
        m_deadP2 = false;
        runPrediction(layer, player, other, mode, clickBothPlayers);
        player->setVisible(false);
        other->setVisible(false);

        clearActivatedObjects();
        m_actions.clear();
        layer->m_gameState = gameState;
        layer->m_effectManager->loadFromState(effectState);
    }

    void Trajectory::drawHitbox(PlayerObject* player) {
        if (!player || !m_node || !m_layer) return;
        auto const zoom = std::max(0.01f, m_layer->m_gameState.m_cameraZoom);
        auto const width = settings::hitboxWidth() / zoom;
        auto const outerColor = settings::color("player-color");
        auto const innerColor = settings::color("player-inner-color");
        auto const rotatedColor = settings::color("player-rotated-color");
        auto const outer = shrink(player->getObjectRect(), width);
        auto const inner = shrink(player->getObjectRect(0.3f, 0.3f), width);
        drawRotatedRect(m_node, outer, player->getRotation(), rotatedColor, width);
        m_node->drawRect(outer, {0.f, 0.f, 0.f, 0.f}, width, outerColor);
        m_node->drawRect(inner, {0.f, 0.f, 0.f, 0.f}, width, innerColor);
    }

    void Trajectory::handlePortal(PlayerObject* player, GameObject* object) {
        if (!isFakePlayer(player) || !object) return;
        rememberActivatedObject(static_cast<EnhancedGameObject*>(object), player);
    }

    void Trajectory::update(GJBaseGameLayer* layer) {
        if (!layer || layer != m_layer || !m_node || !m_fakePlayer1 || !m_fakePlayer2) return;
        m_fakePlayer1->setVisible(false);
        m_fakePlayer2->setVisible(false);

        if (auto* editor = LevelEditorLayer::get(); editor && editor->m_playbackMode == PlaybackMode::Not) {
            m_node->clear();
            m_node->setVisible(false);
            m_calculated = false;
            return;
        }

        if (!settings::showTrajectory()) {
            m_node->clear();
            m_node->setVisible(false);
            m_calculated = false;
            return;
        }

        m_node->setVisible(true);
        auto const signature = computeSignature(layer);
        if (m_calculated && signature == m_lastSignature) return;

        m_drawing = true;
        m_node->clear();
        m_physicsDt = 1.f / static_cast<float>(settings::trajectoryTps());
        m_playerDelta = m_physicsDt * 60.f;

        auto const bothPlayers = !layer->m_levelSettings->m_twoPlayerMode;
        if (layer->m_player1) {
            simulate(layer, true, Hold, bothPlayers);
            simulate(layer, true, Release, bothPlayers);
        }
        if (layer->m_player2 && layer->m_gameState.m_isDualMode &&
            layer->m_levelSettings->m_twoPlayerMode) {
            simulate(layer, false, Hold, false);
            simulate(layer, false, Release, false);
        }

        m_fakePlayer1->setVisible(false);
        m_fakePlayer2->setVisible(false);
        m_lastSignature = signature;
        m_calculated = true;
        m_drawing = false;
    }
}
