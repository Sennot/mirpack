#include "spout_sender.hpp"

#include "overlay.hpp"
#include "settings.hpp"
#include "spout_bridge.hpp"

#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/cocos/platform/win32/CCGL.h>

using namespace geode::prelude;

namespace cleanfeed {
    namespace {
        struct OpenGLState final {
            GLint readFbo = 0;
            GLint drawFbo = 0;
            GLint readBuffer = 0;
            GLint drawBuffer = 0;
            GLint activeTexture = 0;
            GLint texture2D = 0;

            OpenGLState() {
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                glGetIntegerv(GL_READ_BUFFER, &readBuffer);
                glGetIntegerv(GL_DRAW_BUFFER, &drawBuffer);
                glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2D);
            }

            ~OpenGLState() {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
                glReadBuffer(static_cast<GLenum>(readBuffer));
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
                glDrawBuffer(static_cast<GLenum>(drawBuffer));
                glActiveTexture(static_cast<GLenum>(activeTexture));
                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2D));
            }
        };
    }

    SpoutSender& SpoutSender::get() {
        static auto* instance = new SpoutSender();
        return *instance;
    }

    void SpoutSender::captureBackBuffer() {
        auto* gameLayer = GJBaseGameLayer::get();
        if (!settings::enabled() || !gameLayer || gameLayer != overlay::layer()) {
            releaseSender();
            return;
        }

        GLint viewport[4]{};
        glGetIntegerv(GL_VIEWPORT, viewport);
        auto const width = static_cast<unsigned int>(std::max(0, viewport[2]));
        auto const height = static_cast<unsigned int>(std::max(0, viewport[3]));
        if (!width || !height) return;

        auto const desiredName = settings::senderName();
        if (desiredName != m_senderName) {
            releaseSender();
            m_senderName = desiredName;
            if (!m_spout) m_spout = bridge::create();
            bridge::setSenderName(m_spout, m_senderName.c_str());
        }

        OpenGLState const state;
        auto const captureFbo = static_cast<GLuint>(state.drawFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, captureFbo);
        glReadBuffer(captureFbo == 0 ? GL_BACK : static_cast<GLenum>(state.drawBuffer));

        auto const frameBefore = bridge::senderFrame(m_spout);
        auto const sent = bridge::sendFbo(m_spout, captureFbo, width, height, true);
        auto const frameAfter = bridge::senderFrame(m_spout);

        if (!sent) {
            if (!m_warnedSendFailure) {
                log::error("Spout2 rejected the clean-feed framebuffer");
                m_warnedSendFailure = true;
            }
            return;
        }

        m_warnedSendFailure = false;
        if (frameAfter != frameBefore) {
            m_stalledFrames = 0;
            if (!m_loggedPublishing) {
                log::info(
                    "Spout2 sender '{}' is publishing {}x{} frames (share mode {})",
                    m_senderName, width, height, bridge::shareMode(m_spout)
                );
                m_loggedPublishing = true;
            }
        } else if (++m_stalledFrames == 120) {
            log::error(
                "Spout2 sender '{}' exists but its frame counter is not advancing",
                m_senderName
            );
        }
    }

    void SpoutSender::releaseSender() {
        if (!m_senderName.empty()) bridge::releaseSender(m_spout);
        m_senderName.clear();
        m_warnedSendFailure = false;
        m_loggedPublishing = false;
        m_stalledFrames = 0;
    }

    void SpoutSender::shutdown() {
        releaseSender();
        bridge::destroy(m_spout);
        m_spout = nullptr;
    }
}
