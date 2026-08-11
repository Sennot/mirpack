#pragma once

namespace cleanfeed::bridge {
    void* create();
    void destroy(void* handle);
    void setSenderName(void* handle, char const* name);
    void releaseSender(void* handle);
    bool sendTexture(
        void* handle,
        unsigned int texture,
        unsigned int target,
        unsigned int width,
        unsigned int height,
        bool invert,
        unsigned int hostFbo
    );
}
