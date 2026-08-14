// Physics/prediction behavior adapted from Silicate commit f183dbc5
// (GPL-3.0). See LICENSE and THIRD_PARTY_NOTICES.md.
#pragma once

#include <Geode/Geode.hpp>

#include <functional>
#include <unordered_set>

namespace cleanfeed {
    class TrajectoryDrawNode final : public cocos2d::CCDrawNode {
    public:
        static TrajectoryDrawNode* create();
    };

    class Trajectory final {
    public:
        enum Mode {
            Hold = 0x1,
            Swift = 0x2,
            Release = 0x4,
            Left = 0x8,
            Right = 0x10,
            Player1 = 0x20,
            Player2 = 0x40,
        };

        static Trajectory& get();

        void init(GJBaseGameLayer* layer);
        void shutdown(GJBaseGameLayer* layer);
        void update(GJBaseGameLayer* layer);
        void handleButton(bool player1, bool holding);
        void handlePortal(PlayerObject* player, GameObject* object);
        void rememberActivatedObject(EnhancedGameObject* object, PlayerObject* player);

        bool isFakePlayer(PlayerObject* player) const;
        bool drawing() const;
        bool hasDied(PlayerObject* player);
        bool playerHasActivated(PlayerObject* player, EnhancedGameObject* object) const;
        bool realPlayerHasActivated(PlayerObject* player, EnhancedGameObject* object) const;
        PlayerObject* getOtherPlayer(PlayerObject* player) const;

    private:
        struct Signature {
            float p1[7]{};
            float p2[7]{};
            uint64_t p1Flags = 0;
            uint64_t p2Flags = 0;
            float timeWarp = 0.f;
            float cameraZoom = 0.f;
            float lineWidth = 0.f;
            float length = 0.f;
            int tps = 0;
            uint32_t colors[2]{};
            uint32_t boolPack = 0;

            bool operator==(Signature const&) const = default;
        };

        struct Action {
            int delay = 0;
            std::function<void()> function;
            bool executed = false;
        };

        PlayerObject* createFakePlayer(GJBaseGameLayer* layer, std::string const& id);
        Signature computeSignature(GJBaseGameLayer* layer) const;
        void simulate(GJBaseGameLayer* layer, bool player1, int mode, bool clickBothPlayers);
        void runPrediction(
            GJBaseGameLayer* layer,
            PlayerObject* player,
            PlayerObject* other,
            int mode,
            bool both
        );
        bool iterate(
            GJBaseGameLayer* layer,
            PlayerObject* player,
            int mode,
            cocos2d::ccColor4F const& color,
            int& stepCount
        );
        void drawHitbox(PlayerObject* player);
        void clearActivatedObjects();

        GJBaseGameLayer* m_layer = nullptr;
        geode::WeakRef<GJBaseGameLayer> m_layerLifetime;
        TrajectoryDrawNode* m_node = nullptr;
        geode::WeakRef<TrajectoryDrawNode> m_nodeLifetime;
        PlayerObject* m_fakePlayer1 = nullptr;
        PlayerObject* m_fakePlayer2 = nullptr;
        std::unordered_set<uintptr_t> m_activatedObjectsP1;
        std::unordered_set<uintptr_t> m_activatedObjectsP2;
        std::vector<Action> m_actions;
        Signature m_lastSignature;
        bool m_calculated = false;
        bool m_drawing = false;
        bool m_deadP1 = false;
        bool m_deadP2 = false;
        bool m_p1Holding = false;
        bool m_p2Holding = false;
        float m_physicsDt = 1.f / 240.f;
        float m_playerDelta = 0.25f;
        float m_trajectoryWidth = 0.65f;
    };
}
