#pragma once

#include <Geode/Geode.hpp>

namespace cleanfeed {
    class SpoutSender final {
    public:
        static SpoutSender& get();

        void captureBackBuffer();
        void shutdown();

    private:
        SpoutSender() = default;

        bool ensureTarget(unsigned int width, unsigned int height);
        void destroyTarget();
        void releaseSender();

        void* m_spout = nullptr;
        std::string m_senderName;
        GLuint m_fbo = 0;
        GLuint m_texture = 0;
        unsigned int m_width = 0;
        unsigned int m_height = 0;
        bool m_warnedIncomplete = false;
        bool m_warnedSendFailure = false;
    };
}
