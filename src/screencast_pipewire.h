#pragma once

#include "broportal/types.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace broportal {

class PipeWireStreamNode {
public:
    PipeWireStreamNode(const std::string& name, int width = 1920, int height = 1080);
    ~PipeWireStreamNode();

    PipeWireStreamNode(const PipeWireStreamNode&) = delete;
    PipeWireStreamNode& operator=(const PipeWireStreamNode&) = delete;

    bool initialize();
    void close();

    uint32_t node_id() const noexcept { return node_id_; }
    uint64_t serial() const noexcept { return serial_; }
    Coord2D size() const noexcept { return size_; }
    Coord2D position() const noexcept { return position_; }

private:
    std::string name_;
    Coord2D size_{1920, 1080};
    Coord2D position_{0, 0};
    uint32_t node_id_ = 0;
    uint64_t serial_ = 0;

    struct pw_thread_loop* thread_loop_ = nullptr;
    struct pw_context* context_ = nullptr;
    struct pw_core* core_ = nullptr;
    struct pw_stream* stream_ = nullptr;
    struct pw_stream_events stream_events_{};

    static void on_state_changed(
        void* data,
        enum pw_stream_state old_state,
        enum pw_stream_state state,
        const char* error);
};

} // namespace broportal
