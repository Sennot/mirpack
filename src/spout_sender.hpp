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

        void releaseSender();

        void* m_spout = nullptr;
        std::string m_senderName;
        bool m_warnedSendFailure = false;
        bool m_loggedPublishing = false;
        unsigned int m_stalledFrames = 0;
    };
}
