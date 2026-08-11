#include "spout_bridge.hpp"

#include <Spout.h>

namespace cleanfeed::bridge {
    void* create() {
        return new Spout();
    }

    void destroy(void* handle) {
        delete static_cast<Spout*>(handle);
    }

    void setSenderName(void* handle, char const* name) {
        if (handle) static_cast<Spout*>(handle)->SetSenderName(name);
    }

    void releaseSender(void* handle) {
        if (handle) static_cast<Spout*>(handle)->ReleaseSender();
    }

    bool sendTexture(
        void* handle,
        unsigned int texture,
        unsigned int target,
        unsigned int width,
        unsigned int height,
        bool invert,
        unsigned int hostFbo
    ) {
        return handle && static_cast<Spout*>(handle)->SendTexture(
            texture, target, width, height, invert, hostFbo
        );
    }
}
