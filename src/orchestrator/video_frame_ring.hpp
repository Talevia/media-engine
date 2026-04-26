/*
 * VideoFrameRing — bounded SPSC handoff from the Player's video
 * producer thread to its pacer thread.
 *
 * Holds up to `capacity` (RGBA frame, timeline-time) pairs. push()
 * blocks while full unless closed; pop() blocks while empty unless
 * closed. clear() drops every queued frame (used on seek). close()
 * unblocks any waiters and refuses further push/pop — used at
 * destroy.
 *
 * Single-producer / single-consumer in steady state, but mutex +
 * two condition_variables (not_full / not_empty) keeps the
 * implementation small and matches the RenderThread pattern in
 * src/gpu/render_thread.hpp.
 */
#pragma once

#include "graph/types.hpp"
#include "media_engine/types.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

namespace me::orchestrator {

struct VideoFrameSlot {
    me_rational_t                                 present_at{0, 1};
    std::shared_ptr<me::graph::RgbaFrameData>     rgba;
    /* The Player's seek_epoch_ at the time the producer captured
     * its cursor for this frame. The pacer drops slots whose epoch
     * doesn't match the current seek_epoch_ — covers the
     * narrow race where a seek arrives between the producer's
     * post-await stale-check and the push, leaving a frame for
     * the old cursor in a ring that the seek already cleared. */
    int64_t                                       seek_epoch = 0;
};

class VideoFrameRing {
public:
    explicit VideoFrameRing(std::size_t capacity);

    /* Push a frame; blocks until space is available or close() fires.
     * Returns false iff the ring was closed before the push landed. */
    bool push(VideoFrameSlot slot);

    /* Pop the oldest frame; blocks until one is available or close()
     * fires. On close() with empty ring returns false. */
    bool pop(VideoFrameSlot* out);

    /* Drop everything queued (seek invalidation). Producer wakes if
     * it was blocked on full. */
    void clear();

    /* Permanent shutdown; any pending push/pop returns false. */
    void close();

    std::size_t size() const;
    std::size_t capacity() const noexcept { return capacity_; }

private:
    mutable std::mutex             mu_;
    std::condition_variable        not_full_;
    std::condition_variable        not_empty_;
    std::deque<VideoFrameSlot>     q_;
    std::size_t                    capacity_;
    bool                           closed_ = false;
};

}  // namespace me::orchestrator
