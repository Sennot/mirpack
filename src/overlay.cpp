#include "overlay.hpp"

#include "render_pass.hpp"

using namespace geode::prelude;

namespace cleanfeed::overlay {
    namespace {
        class OverlayRoot final : public cocos2d::CCNode {
        public:
            static OverlayRoot* create() {
                auto* result = new OverlayRoot();
                if (result && result->init()) {
                    result->autorelease();
                    return result;
                }
                CC_SAFE_DELETE(result);
                return nullptr;
            }

            void visit() override {
                if (render::isPlayerOverlayPass()) {
                    cocos2d::CCNode::visit();
                }
            }
        };

        cocos2d::CCNode* s_root = nullptr;
        GJBaseGameLayer* s_layer = nullptr;
    }

    void attach(GJBaseGameLayer* gameLayer) {
        if (!gameLayer || !gameLayer->m_objectLayer) return;

        if (s_root && s_root->getParent()) {
            s_root->removeFromParent();
        }

        s_root = OverlayRoot::create();
        s_layer = gameLayer;
        s_root->setID("overlay-root"_spr);
        gameLayer->addChild(s_root, 100000);
    }

    void detach(GJBaseGameLayer* gameLayer) {
        if (gameLayer != s_layer) return;
        s_root = nullptr;
        s_layer = nullptr;
    }

    void drawForPlayer() {
        if (!s_root || !s_layer || !s_root->getParent() || !s_layer->m_objectLayer) return;

        kmGLMatrixMode(KM_GL_MODELVIEW);
        kmGLPushMatrix();

        auto const objectLayerTransform = s_layer->m_objectLayer->nodeToWorldTransform();
        kmMat4 matrix{{
            objectLayerTransform.a, objectLayerTransform.b, 0.f, 0.f,
            objectLayerTransform.c, objectLayerTransform.d, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            objectLayerTransform.tx, objectLayerTransform.ty, 0.f, 1.f,
        }};
        kmGLMultMatrix(&matrix);

        {
            render::PlayerOverlayPass overlayPass;
            s_root->visit();
        }

        kmGLPopMatrix();
    }

    cocos2d::CCNode* root() {
        return s_root;
    }

    GJBaseGameLayer* layer() {
        return s_layer;
    }
}
