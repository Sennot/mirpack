#include "xdbot_filter.hpp"

#include "settings.hpp"
#include "ui_visibility.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCScene.hpp>

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <limits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace cleanfeed::xdbot_filter {
    namespace {
        enum class NodeKind { UI, Leaf, Game, Xdbot };

        struct DeferredNode {
            Ref<CCNode> node;
            WeakRef<CCNode> parent;
            WeakRef<CCScene> scene;
            kmMat4 modelView;
            kmMat4 projection;
        };

        std::vector<DeferredNode> s_deferred;
        std::vector<CCNode*> s_levelContainers;
        std::unordered_map<std::type_info const*, NodeKind> s_types;
        uintptr_t s_moduleBegin = 0;
        uintptr_t s_moduleEnd = 0;
        unsigned int s_frameDepth = 0;
        bool s_localPass = false;

        bool findModule() {
            if (s_moduleBegin) return true;
            auto module = GetModuleHandleW(L"zilko.xdbot.dll");
            if (!module) return false;
            MODULEINFO info{};
            if (!K32GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return false;
            s_moduleBegin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
            s_moduleEnd = s_moduleBegin + info.SizeOfImage;
            log::info("Spout UI filter found zilko.xdbot.dll; local UI stays enabled");
            return true;
        }

        NodeKind kind(CCNode* node) {
            auto* type = &typeid(*node);
            if (auto found = s_types.find(type); found != s_types.end()) return found->second;

            // A custom popup's RTTI descriptor lives in its defining DLL. This
            // covers xDBot's own popup classes even when they have no node ID,
            // without fixed addresses, binary patches or class-name guesses.
            auto const address = reinterpret_cast<uintptr_t>(type);
            auto result = NodeKind::UI;
            if (typeinfo_cast<GJBaseGameLayer*>(node)) result = NodeKind::Game;
            else if (address >= s_moduleBegin && address < s_moduleEnd) result = NodeKind::Xdbot;
            else if (typeinfo_cast<CCSprite*>(node) || typeinfo_cast<CCSpriteBatchNode*>(node) ||
                     typeinfo_cast<CCParticleSystem*>(node) || typeinfo_cast<CCClippingNode*>(node) ||
                     typeinfo_cast<CCScrollLayerExt*>(node)) result = NodeKind::Leaf;
            s_types.emplace(type, result);
            return result;
        }

        kmMat4 matrixFor(CCAffineTransform const& transform) {
            return kmMat4{{
                transform.a, transform.b, 0.f, 0.f,
                transform.c, transform.d, 0.f, 0.f,
                0.f, 0.f, 1.f, 0.f,
                transform.tx, transform.ty, 0.f, 1.f,
            }};
        }

        void restoreVisibility() {
            for (auto const& entry : s_deferred) entry.node->setVisible(true);
        }

        void collectScene(CCScene* scene) {
            if (!s_frameDepth || s_localPass || !settings::enabled() || !settings::hideXdbotUI() ||
                !findModule()) return;

            // Discover each layer in this scene, including the outgoing layer
            // during editor/play transitions after our gameplay overlay detached.
            // Reuse capacity, but keep no level pointers between traversals.
            s_levelContainers.clear();
            auto isLevelContainer = [](CCNode* node) {
                return std::find(s_levelContainers.begin(), s_levelContainers.end(), node) !=
                    s_levelContainers.end();
            };
            kmMat4 sceneView;
            kmMat4 projection;
            kmGLGetMatrix(KM_GL_MODELVIEW, &sceneView);
            kmGLGetMatrix(KM_GL_PROJECTION, &projection);
            auto const worldToScene = scene->worldToNodeTransform();

            ui_visibility::collect<CCNode>(scene,
                [isLevelContainer](CCNode* node) {
                    // Never classify the actual level or its object container as
                    // UI, regardless of a third-party ID or subclass.
                    if (isLevelContainer(node)) return false;
                    auto const nodeKind = kind(node);
                    if (nodeKind == NodeKind::Game) {
                        s_levelContainers.push_back(static_cast<GJBaseGameLayer*>(node)->m_objectLayer);
                        return false;
                    }
                    return nodeKind == NodeKind::Xdbot ||
                        ui_visibility::isXdbotID(std::string_view(node->getID()));
                },
                [isLevelContainer](CCNode* node) {
                    return isLevelContainer(node) || kind(node) == NodeKind::Leaf;
                },
                [](CCNode* node) {
                    node->sortAllChildren();
                    return CCArrayExt<CCNode*>(node->getChildren());
                },
                [&](CCNode* node) {
                    auto* parent = node->getParent();
                    if (!parent) return;
                    auto const parentToScene = CCAffineTransformConcat(
                        parent->nodeToWorldTransform(), worldToScene
                    );
                    auto const relative = matrixFor(parentToScene);
                    kmMat4 view;
                    kmMat4Multiply(&view, &sceneView, &relative);
                    // Retain only for this render pass; no nodes survive scene
                    // teardown through a persistent raw-pointer cache.
                    s_deferred.push_back({node, parent, scene, view, projection});
                    node->setVisible(false);
                }
            );
            s_levelContainers.clear();
        }

        class SceneBoundary final : public CCNode {
        public:
            static SceneBoundary* create() {
                auto* result = new SceneBoundary;
                if (result->init()) {
                    result->autorelease();
                    return result;
                }
                delete result;
                return nullptr;
            }

            void visit() override {
                collectScene(static_cast<CCScene*>(getParent()));
            }
        };

        void attachBoundary(CCScene* scene) {
            if (!scene || scene->getChildByID("xdbot-filter-boundary"_spr)) return;
            if (auto* node = SceneBoundary::create()) {
                node->setID("xdbot-filter-boundary"_spr);
                // A negative boundary leaves getHighestChildZ() unchanged, so
                // popup ordering and its highest-Z + 1 convention still work.
                scene->addChild(node, std::numeric_limits<int>::min());
            }
        }

        class FrameScope final {
        public:
            FrameScope() { ++s_frameDepth; }
            ~FrameScope() {
                if (--s_frameDepth == 0) {
                    // Also restore if another renderer skipped swapBuffers.
                    restoreVisibility();
                    s_deferred.clear();
                }
            }
        };
    }

    void drawForPlayer() {
        if (s_deferred.empty() || s_localPass) return;
        struct LocalPassScope {
            LocalPassScope() { s_localPass = true; }
            ~LocalPassScope() { s_localPass = false; }
        } localPass;
        restoreVisibility();
        kmGLMatrixMode(KM_GL_PROJECTION);
        kmGLPushMatrix();
        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPushMatrix();

        for (auto const& entry : s_deferred) {
            auto parent = entry.parent.lock();
            auto scene = entry.scene.lock();
            if (!parent || !scene || entry.node->getParent() != parent.data() ||
                !ui_visibility::visibleUnder<CCNode>(entry.node.data(), scene.data())) continue;
            kmGLMatrixMode(KM_GL_PROJECTION);
            kmGLLoadMatrix(&entry.projection);
            kmGLMatrixMode(KM_GL_MODELVIEW);
            kmGLLoadMatrix(&entry.modelView);
            entry.node->visit();
        }

        kmGLMatrixMode(KM_GL_PROJECTION);
        kmGLPopMatrix();
        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPopMatrix();
        s_deferred.clear();
    }

    class $modify(CleanFeedXdbotScene, CCScene) {
        bool init() override {
            if (!CCScene::init()) return false;
            attachBoundary(this);
            return true;
        }
    };

    class $modify(CleanFeedXdbotDirector, CCDirector) {
        void drawScene() {
            FrameScope frame;
            // Covers a scene created before this mod's hooks were installed.
            static WeakRef<CCScene> lastScene;
            auto* scene = getRunningScene();
            if (lastScene.lock().data() != scene) {
                attachBoundary(scene);
                lastScene = scene;
            }
            CCDirector::drawScene();
        }
    };
}
