#include "orchestrator/video_frame_ring.hpp"

#include <utility>

namespace me::orchestrator {

VideoFrameRing::VideoFrameRing(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

bool VideoFrameRing::push(VideoFrameSlot slot) {
    std::unique_lock<std::mutex> lk(mu_);
    not_full_.wait(lk, [this] { return closed_ || q_.size() < capacity_; });
    if (closed_) return false;
    q_.push_back(std::move(slot));
    not_empty_.notify_one();
    return true;
}

bool VideoFrameRing::pop(VideoFrameSlot* out) {
    std::unique_lock<std::mutex> lk(mu_);
    not_empty_.wait(lk, [this] { return closed_ || !q_.empty(); });
    if (q_.empty()) return false;   /* closed + drained */
    *out = std::move(q_.front());
    q_.pop_front();
    not_full_.notify_one();
    return true;
}

void VideoFrameRing::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    q_.clear();
    not_full_.notify_all();
}

void VideoFrameRing::close() {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
    not_full_.notify_all();
    not_empty_.notify_all();
}

std::size_t VideoFrameRing::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
}

}  // namespace me::orchestrator
