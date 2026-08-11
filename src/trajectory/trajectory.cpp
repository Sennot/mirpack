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
#include <chrono>
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
        m_releaseBranch.node = TrajectoryDrawNode::create();
        m_holdBranch.node = TrajectoryDrawNode::create();
        if (!m_releaseBranch.node || !m_holdBranch.node) {
            m_layer = nullptr;
            m_releaseBranch = {};
            m_holdBranch = {};
            return;
        }

        m_releaseBranch.node->setID("trajectory-release"_spr);
        m_releaseBranch.node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        overlay::root()->addChild(m_releaseBranch.node, 20);

        m_holdBranch.node->setID("trajectory-hold"_spr);
        m_holdBranch.node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        overlay::root()->addChild(m_holdBranch.node, 21);

        m_fakePlayer1 = createFakePlayer(layer, "trajectory-fake-player-1"_spr);
        m_fakePlayer2 = createFakePlayer(layer, "trajectory-fake-player-2"_spr);
        m_holdBranch.calculated = false;
        m_releaseBranch.calculated = false;
        m_p1Holding = false;
        m_p2Holding = false;
        m_forceRefresh = true;
        m_forcedMode = Release;
    }

    void Trajectory::shutdown(GJBaseGameLayer* layer) {
        if (m_layer && layer && layer != m_layer) return;

        if (m_fakePlayer1 && m_fakePlayer1->getParent()) m_fakePlayer1->removeFromParent();
        if (m_fakePlayer2 && m_fakePlayer2->getParent()) m_fakePlayer2->removeFromParent();
        if (m_holdBranch.node && m_holdBranch.node->getParent()) {
            m_holdBranch.node->removeFromParent();
        }
        if (m_releaseBranch.node && m_releaseBranch.node->getParent()) {
            m_releaseBranch.node->removeFromParent();
        }

        m_layer = nullptr;
        m_holdBranch = {};
        m_releaseBranch = {};
        m_activeNode = nullptr;
        m_fakePlayer1 = nullptr;
        m_fakePlayer2 = nullptr;
        m_actions.clear();
        clearActivatedObjects();
        m_drawing = false;
        m_forceRefresh = true;
        m_forcedMode = Release;
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
        auto& previous = player1 ? m_p1Holding : m_p2Holding;
        if (previous == holding) return;
        previous = holding;
        m_forceRefresh = true;
        m_forcedMode = holding ? Hold : Release;
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

    cocos2d::CCPoint Trajectory::currentAnchor(GJBaseGameLayer* layer) const {
        if (!layer) return {};
        if (layer->m_player1) return layer->m_player1->getPosition();
        if (layer->m_player2) return layer->m_player2->getPosition();
        return {};
    }

    bool Trajectory::needsImmediateRefresh(
        Signature const& current,
        Signature const& previous
    ) const {
        if (current.p1Flags != previous.p1Flags ||
            current.p2Flags != previous.p2Flags ||
            current.boolPack != previous.boolPack ||
            current.timeWarp != previous.timeWarp ||
            current.cameraZoom != previous.cameraZoom ||
            current.lineWidth != previous.lineWidth ||
            current.length != previous.length ||
            current.tps != previous.tps ||
            current.colors[0] != previous.colors[0] ||
            current.colors[1] != previous.colors[1] ||
            current.p1[5] != previous.p1[5] ||
            current.p2[5] != previous.p2[5] ||
            current.p1[6] != previous.p1[6] ||
            current.p2[6] != previous.p2[6]) {
            return true;
        }

        auto const movedFar = [](float const (&now)[7], float const (&before)[7]) {
            auto const dx = now[0] - before[0];
            auto const dy = now[1] - before[1];
            return dx * dx + dy * dy > 48.f * 48.f ||
                   std::abs(now[3] - before[3]) > 4.f ||
                   std::abs(now[4] - before[4]) > 4.f;
        };
        return movedFar(current.p1, previous.p1) ||
               movedFar(current.p2, previous.p2);
    }

    void Trajectory::positionBranch(
        BranchState& branch,
        cocos2d::CCPoint const& anchor
    ) {
        if (!branch.node) return;
        branch.node->setPosition(branch.calculated ? anchor - branch.anchor : cocos2d::CCPoint{});
    }

    void Trajectory::calculateBranch(
        GJBaseGameLayer* layer,
        BranchState& branch,
        Signature const& signature,
        int mode,
        bool active
    ) {
        if (!branch.node) return;

        auto const started = std::chrono::steady_clock::now();
        auto const anchor = currentAnchor(layer);
        branch.node->setPosition({});
        branch.node->clear();
        m_activeNode = branch.node;
        m_drawing = true;
        m_physicsDt = 1.f / static_cast<float>(settings::trajectoryTps());
        m_playerDelta = m_physicsDt * 60.f;

        auto const bothPlayers = !layer->m_levelSettings->m_twoPlayerMode;
        if (layer->m_player1) simulate(layer, true, mode, bothPlayers);
        if (layer->m_player2 && layer->m_gameState.m_isDualMode &&
            layer->m_levelSettings->m_twoPlayerMode) {
            simulate(layer, false, mode, false);
        }

        m_fakePlayer1->setVisible(false);
        m_fakePlayer2->setVisible(false);
        m_drawing = false;
        m_activeNode = nullptr;

        branch.anchor = anchor;
        branch.signature = signature;
        branch.calculated = true;

        auto const finished = std::chrono::steady_clock::now();
        auto const cost = finished - started;
        auto const basePeriod = std::chrono::duration_cast<
            std::chrono::steady_clock::duration
        >(std::chrono::duration<double>(active ? 1.0 / 30.0 : 1.0 / 15.0));
        branch.nextRefresh = started + std::max(basePeriod, cost * 3);
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
        auto const branchScale = (mode & Release) ? 1.4f : 0.72f;
        m_activeNode->drawSegment(
            previousPosition, player->getPosition(),
            settings::trajectoryWidth() * branchScale / zoom, color
        );
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
        if (!player || !m_activeNode || !m_layer) return;
        auto const zoom = std::max(0.01f, m_layer->m_gameState.m_cameraZoom);
        auto const width = settings::hitboxWidth() / zoom;
        auto const outerColor = settings::color("player-color");
        auto const innerColor = settings::color("player-inner-color");
        auto const rotatedColor = settings::color("player-rotated-color");
        auto const outer = shrink(player->getObjectRect(), width);
        auto const inner = shrink(player->getObjectRect(0.3f, 0.3f), width);
        drawRotatedRect(m_activeNode, outer, player->getRotation(), rotatedColor, width);
        m_activeNode->drawRect(outer, {0.f, 0.f, 0.f, 0.f}, width, outerColor);
        m_activeNode->drawRect(inner, {0.f, 0.f, 0.f, 0.f}, width, innerColor);
    }

    void Trajectory::handlePortal(PlayerObject* player, GameObject* object) {
        if (!isFakePlayer(player) || !object) return;
        rememberActivatedObject(static_cast<EnhancedGameObject*>(object), player);
    }

    void Trajectory::update(GJBaseGameLayer* layer) {
        if (!layer || layer != m_layer || !m_holdBranch.node ||
            !m_releaseBranch.node || !m_fakePlayer1 || !m_fakePlayer2) {
            return;
        }
        m_fakePlayer1->setVisible(false);
        m_fakePlayer2->setVisible(false);

        if (auto* editor = LevelEditorLayer::get(); editor && editor->m_playbackMode == PlaybackMode::Not) {
            m_holdBranch.node->clear();
            m_releaseBranch.node->clear();
            m_holdBranch.node->setVisible(false);
            m_releaseBranch.node->setVisible(false);
            m_holdBranch.calculated = false;
            m_releaseBranch.calculated = false;
            m_forceRefresh = true;
            return;
        }

        if (!settings::showTrajectory()) {
            m_holdBranch.node->clear();
            m_releaseBranch.node->clear();
            m_holdBranch.node->setVisible(false);
            m_releaseBranch.node->setVisible(false);
            m_holdBranch.calculated = false;
            m_releaseBranch.calculated = false;
            m_forceRefresh = true;
            return;
        }

        m_holdBranch.node->setVisible(true);
        m_releaseBranch.node->setVisible(true);
        auto const signature = computeSignature(layer);
        auto const anchor = currentAnchor(layer);
        positionBranch(m_holdBranch, anchor);
        positionBranch(m_releaseBranch, anchor);

        auto& inputBranch = m_p1Holding ? m_holdBranch : m_releaseBranch;
        auto& alternateBranch = m_p1Holding ? m_releaseBranch : m_holdBranch;
        auto const inputMode = m_p1Holding ? Hold : Release;
        auto const alternateMode = m_p1Holding ? Release : Hold;
        auto const now = std::chrono::steady_clock::now();

        BranchState* selected = nullptr;
        int selectedMode = 0;
        bool selectedIsActive = false;

        if (m_forceRefresh) {
            selectedMode = m_forcedMode;
            selected = selectedMode == Hold ? &m_holdBranch : &m_releaseBranch;
            selectedIsActive = selectedMode == inputMode;
        } else if (!inputBranch.calculated) {
            selected = &inputBranch;
            selectedMode = inputMode;
            selectedIsActive = true;
        } else if (!alternateBranch.calculated) {
            selected = &alternateBranch;
            selectedMode = alternateMode;
        } else {
            auto const inputChanged = !(signature == inputBranch.signature);
            auto const alternateChanged = !(signature == alternateBranch.signature);
            auto const inputUrgent = inputChanged &&
                needsImmediateRefresh(signature, inputBranch.signature);
            auto const alternateUrgent = alternateChanged &&
                needsImmediateRefresh(signature, alternateBranch.signature);
            auto const inputDue = inputChanged && now >= inputBranch.nextRefresh;
            auto const alternateDue = alternateChanged && now >= alternateBranch.nextRefresh;

            if (inputUrgent) {
                selected = &inputBranch;
                selectedMode = inputMode;
                selectedIsActive = true;
            } else if (alternateUrgent && !inputDue) {
                selected = &alternateBranch;
                selectedMode = alternateMode;
            } else if (inputDue && alternateDue) {
                if (inputBranch.nextRefresh <= alternateBranch.nextRefresh) {
                    selected = &inputBranch;
                    selectedMode = inputMode;
                    selectedIsActive = true;
                } else {
                    selected = &alternateBranch;
                    selectedMode = alternateMode;
                }
            } else if (inputDue) {
                selected = &inputBranch;
                selectedMode = inputMode;
                selectedIsActive = true;
            } else if (alternateDue) {
                selected = &alternateBranch;
                selectedMode = alternateMode;
            }
        }

        if (!selected) return;
        calculateBranch(layer, *selected, signature, selectedMode, selectedIsActive);
        m_forceRefresh = false;
    }
}
