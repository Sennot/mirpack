#include "spout_sender.hpp"

#include "overlay.hpp"
#include "settings.hpp"
#include "spout_bridge.hpp"

#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/cocos/platform/win32/CCGL.h>

using namespace geode::prelude;

namespace cleanfeed {
    SpoutSender& SpoutSender::get() {
        static auto* instance = new SpoutSender();
        return *instance;
    }

    bool SpoutSender::ensureTarget(unsigned int width, unsigned int height) {
        if (m_fbo && m_texture && width == m_width && height == m_height) return true;

        destroyTarget();
        if (!width || !height) return false;

        GLint previousFbo = 0;
        GLint previousTexture = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glGenFramebuffers(1, &m_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);

        auto const complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));

        if (!complete) {
            if (!m_warnedIncomplete) {
                log::error("Unable to create the clean-feed OpenGL framebuffer");
                m_warnedIncomplete = true;
            }
            destroyTarget();
            return false;
        }

        m_width = width;
        m_height = height;
        m_warnedIncomplete = false;
        log::info("Spout2 clean-feed target created: {}x{}", width, height);
        return true;
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
        if (!ensureTarget(width, height)) return;

        auto const desiredName = settings::senderName();
        if (desiredName != m_senderName) {
            releaseSender();
            m_senderName = desiredName;
            if (!m_spout) m_spout = bridge::create();
            bridge::setSenderName(m_spout, m_senderName.c_str());
        }

        GLint previousFbo = 0;
        GLint previousTexture = 0;
        GLint previousReadBuffer = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFbo);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);

        glReadBuffer(previousFbo == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glCopyTexSubImage2D(
            GL_TEXTURE_2D, 0, 0, 0,
            viewport[0], viewport[1], static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height)
        );
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
        glReadBuffer(static_cast<GLenum>(previousReadBuffer));

        if (!bridge::sendTexture(
            m_spout, m_texture, GL_TEXTURE_2D, m_width, m_height, true,
            static_cast<unsigned int>(previousFbo)
        )) {
            if (!m_warnedSendFailure) {
                log::warn("Spout2 rejected a clean-feed frame");
                m_warnedSendFailure = true;
            }
        } else {
            m_warnedSendFailure = false;
        }

    }

    void SpoutSender::releaseSender() {
        if (!m_senderName.empty()) bridge::releaseSender(m_spout);
        m_senderName.clear();
        m_warnedSendFailure = false;
    }

    void SpoutSender::destroyTarget() {
        if (m_texture) glDeleteTextures(1, &m_texture);
        if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
        m_texture = 0;
        m_fbo = 0;
        m_width = 0;
        m_height = 0;
    }

    void SpoutSender::shutdown() {
        releaseSender();
        bridge::destroy(m_spout);
        m_spout = nullptr;
        destroyTarget();
    }
}
