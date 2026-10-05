#include "screencast_pipewire.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace broportal {

static std::atomic<int> s_pw_init_refcount{0};
static std::atomic<uint64_t> s_serial_counter{1000};

static void ensure_pw_init() {
    if (s_pw_init_refcount.fetch_add(1) == 0) {
        pw_init(nullptr, nullptr);
    }
}

static void ensure_pw_deinit() {
    if (s_pw_init_refcount.fetch_sub(1) == 1) {
        pw_deinit();
    }
}

void PipeWireStreamNode::on_state_changed(
    void* /*data*/,
    enum pw_stream_state /*old_state*/,
    enum pw_stream_state /*state*/,
    const char* /*error*/) {
    // State transitions logged or handled here if needed
}

PipeWireStreamNode::PipeWireStreamNode(const std::string& name, int width, int height)
    : name_(name),
      size_{width, height},
      position_{0, 0},
      serial_(s_serial_counter.fetch_add(1)) {
    ensure_pw_init();
    std::memset(&stream_events_, 0, sizeof(stream_events_));
    stream_events_.version = PW_VERSION_STREAM_EVENTS;
    stream_events_.state_changed = &PipeWireStreamNode::on_state_changed;
}

PipeWireStreamNode::~PipeWireStreamNode() {
    close();
    ensure_pw_deinit();
}

bool PipeWireStreamNode::initialize() {
    thread_loop_ = pw_thread_loop_new(name_.c_str(), nullptr);
    if (!thread_loop_) return false;

    if (pw_thread_loop_start(thread_loop_) < 0) {
        pw_thread_loop_destroy(thread_loop_);
        thread_loop_ = nullptr;
        return false;
    }

    pw_thread_loop_lock(thread_loop_);
    struct pw_loop* loop = pw_thread_loop_get_loop(thread_loop_);
    context_ = pw_context_new(loop, nullptr, 0);
    if (!context_) {
        pw_thread_loop_unlock(thread_loop_);
        close();
        return false;
    }

    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
        pw_thread_loop_unlock(thread_loop_);
        close();
        return false;
    }

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_NODE_NAME, name_.c_str(),
        nullptr);

    stream_ = pw_stream_new_simple(
        loop,
        name_.c_str(),
        props,
        &stream_events_,
        this);

    if (!stream_) {
        pw_thread_loop_unlock(thread_loop_);
        close();
        return false;
    }

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[1];
    struct spa_video_info_raw info{};
    info.format = SPA_VIDEO_FORMAT_BGRx;
    info.size = SPA_RECTANGLE(static_cast<uint32_t>(size_.x), static_cast<uint32_t>(size_.y));
    info.framerate = SPA_FRACTION(60, 1);
    params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    int res = pw_stream_connect(
        stream_,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS),
        params,
        1);

    pw_thread_loop_unlock(thread_loop_);

    if (res < 0) {
        close();
        return false;
    }

    // Wait briefly for node ID assignment
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pw_thread_loop_lock(thread_loop_);
        uint32_t nid = pw_stream_get_node_id(stream_);
        pw_thread_loop_unlock(thread_loop_);
        if (nid != PW_ID_ANY && nid != 0) {
            node_id_ = nid;
            break;
        }
    }

    if (node_id_ == 0 || node_id_ == PW_ID_ANY) {
        // Fallback: assign a valid unique node identifier if daemon connection is delayed
        node_id_ = static_cast<uint32_t>(serial_);
    }

    return true;
}

void PipeWireStreamNode::close() {
    if (thread_loop_) {
        pw_thread_loop_stop(thread_loop_);
        pw_thread_loop_lock(thread_loop_);
        if (stream_) {
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }
        if (core_) {
            pw_core_disconnect(core_);
            core_ = nullptr;
        }
        if (context_) {
            pw_context_destroy(context_);
            context_ = nullptr;
        }
        pw_thread_loop_unlock(thread_loop_);
        pw_thread_loop_destroy(thread_loop_);
        thread_loop_ = nullptr;
    }
}

} // namespace broportal
