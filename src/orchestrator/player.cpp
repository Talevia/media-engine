#include "orchestrator/player.hpp"

#include "core/engine_impl.hpp"
#include "core/frame_impl.hpp"
#include "graph/eval_context.hpp"
#include "graph/future.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "orchestrator/previewer_graph.hpp"
#include "scheduler/scheduler.hpp"
#include "timeline/timeline_impl.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace me::orchestrator {

namespace {

/* ---- Rational helpers ---------------------------------------------------
 * The arithmetic in this file uses the same cross-multiply pattern as
 * Previewer::frame_at to stay int64 throughout — the surrounding
 * timeline values are already int64 numerators / denominators, and
 * promoting to double here would smuggle in float imprecision against
 * which docs/VISION.md §3.1 explicitly cautions. */

me_rational_t r_add(me_rational_t a, me_rational_t b) {
    if (a.den <= 0) a.den = 1;
    if (b.den <= 0) b.den = 1;
    return me_rational_t{ a.num * b.den + b.num * a.den, a.den * b.den };
}

/* sign(a - b) */
int r_cmp(me_rational_t a, me_rational_t b) {
    if (a.den <= 0) a.den = 1;
    if (b.den <= 0) b.den = 1;
    const __int128 lhs = static_cast<__int128>(a.num) * b.den;
    const __int128 rhs = static_cast<__int128>(b.num) * a.den;
    if (lhs < rhs) return -1;
    if (lhs > rhs) return  1;
    return 0;
}

/* a - b in microseconds, clamped to int64. */
int64_t r_micros_diff(me_rational_t a, me_rational_t b) {
    if (a.den <= 0) a.den = 1;
    if (b.den <= 0) b.den = 1;
    /* (a.num*b.den - b.num*a.den) / (a.den*b.den) seconds → micros via *1e6 */
    const __int128 num = static_cast<__int128>(a.num) * b.den
                       - static_cast<__int128>(b.num) * a.den;
    const __int128 den = static_cast<__int128>(a.den) * b.den;
    if (den == 0) return 0;
    const __int128 micros = (num * 1'000'000) / den;
    if (micros >  9'000'000'000LL) return  9'000'000'000LL;
    if (micros < -9'000'000'000LL) return -9'000'000'000LL;
    return static_cast<int64_t>(micros);
}

/* frame_period from frame_rate. {fps_num, fps_den} → {fps_den, fps_num}. */
me_rational_t frame_period_from_rate(me_rational_t fr) {
    if (fr.num <= 0 || fr.den <= 0) return me_rational_t{1, 30};
    return me_rational_t{ fr.den, fr.num };
}

}  // namespace

/* ----------------------------------------------------------------- ctor */

Player::Player(me_engine*                            engine,
               std::shared_ptr<const me::Timeline>   timeline,
               const me_player_config_t&             cfg)
    : engine_(engine),
      tl_(std::move(timeline)),
      cfg_(cfg),
      clock_(cfg.master_clock == ME_CLOCK_AUTO
                 ? (tl_ && [&]{
                       for (const auto& tr : tl_->tracks) {
                           if (tr.kind == me::TrackKind::Audio) return true;
                       }
                       return false;
                   }() ? ME_CLOCK_AUDIO : ME_CLOCK_WALL)
                 : cfg.master_clock),
      ring_(cfg.video_ring_capacity > 0
                ? static_cast<std::size_t>(cfg.video_ring_capacity)
                : 3) {

    if (tl_) frame_period_ = frame_period_from_rate(tl_->frame_rate);

    /* The clock anchors at t=0 in paused state; play() bumps anchor. */
    clock_.seek(me_rational_t{0, 1});

    producer_ = std::thread(&Player::producer_loop, this);
    pacer_    = std::thread(&Player::pacer_loop, this);
}

/* ----------------------------------------------------------------- dtor */

Player::~Player() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        shutdown_ = true;
    }
    state_cv_.notify_all();
    ring_.close();   /* unblocks producer (push) and pacer (pop) */
    if (producer_.joinable()) producer_.join();
    if (pacer_.joinable())    pacer_.join();
}

/* ---------------------------------------------------------- transport */

me_status_t Player::play(float rate) {
    /* Rate ≠ 1.0 needs SoundTouch tempo + frame skip/repeat — separate
     * milestone. Reject up front so a host doesn't silently end up at
     * 1× when asking for 2×. */
    if (rate != 1.0f) return ME_E_UNSUPPORTED;
    clock_.play(rate);
    {
        std::lock_guard<std::mutex> lk(mu_);
        notify_state_changed_locked();
    }
    return ME_OK;
}

me_status_t Player::pause() {
    clock_.pause();
    {
        std::lock_guard<std::mutex> lk(mu_);
        notify_state_changed_locked();
    }
    return ME_OK;
}

me_status_t Player::seek(me_rational_t time) {
    if (time.den <= 0) time.den = 1;
    if (time.num <  0) time.num = 0;
    clock_.seek(time);
    /* Drop the produced backlog — it's all stale relative to the new
     * playhead. The pacer's "drop if too far behind" check absorbs
     * any frame that escaped through the ring before we got here, so
     * Phase-2 seek correctness doesn't yet rely on cooperative cancel
     * (added in Phase 5). */
    ring_.clear();
    {
        std::lock_guard<std::mutex> lk(mu_);
        produce_cursor_ = time;
        notify_state_changed_locked();
    }
    return ME_OK;
}

me_status_t Player::report_audio_playhead(me_rational_t t) {
    clock_.report_audio_playhead(t);
    return ME_OK;
}

me_rational_t Player::current_time() const { return clock_.current(); }
bool          Player::is_playing()   const { return clock_.is_playing(); }

/* --------------------------------------------------------- callback set */

me_status_t Player::set_video_callback(me_player_video_cb cb, void* user) {
    std::lock_guard<std::mutex> lk(mu_);
    video_cb_   = cb;
    video_user_ = user;
    return ME_OK;
}

me_status_t Player::set_audio_callback(me_player_audio_cb cb, void* user) {
    std::lock_guard<std::mutex> lk(mu_);
    audio_cb_   = cb;
    audio_user_ = user;
    return ME_OK;
}

void Player::notify_state_changed_locked() {
    state_cv_.notify_all();
}

/* ---------------------------------------------------- timeline lookup */

me_status_t Player::resolve_active_clip(me_rational_t       t,
                                         std::string*       out_uri,
                                         me_rational_t*     out_source_t) const {
    if (!tl_ || tl_->tracks.empty() || tl_->clips.empty()) {
        return ME_E_NOT_FOUND;
    }
    if (t.den <= 0) t.den = 1;
    if (t.num <  0) t.num = 0;

    /* Phase-1 single-bottom-track lookup (mirrors Previewer:32-65). */
    const std::string& bottom_id = tl_->tracks[0].id;
    const me::Clip*    active    = nullptr;
    me_rational_t      clip_local{0, 1};

    for (const auto& c : tl_->clips) {
        if (c.track_id != bottom_id) continue;

        const int64_t e_num = c.time_start.num * c.time_duration.den +
                              c.time_duration.num * c.time_start.den;
        const int64_t e_den = c.time_start.den * c.time_duration.den;

        const bool ge_start =
            t.num * c.time_start.den >= c.time_start.num * t.den;
        const bool lt_end =
            t.num * e_den < e_num * t.den;
        if (!ge_start || !lt_end) continue;

        active = &c;
        clip_local = me_rational_t{
            t.num * c.time_start.den - c.time_start.num * t.den,
            t.den * c.time_start.den,
        };
        break;
    }
    if (!active) return ME_E_NOT_FOUND;

    auto a_it = tl_->assets.find(active->asset_id);
    if (a_it == tl_->assets.end()) return ME_E_NOT_FOUND;

    *out_uri = a_it->second.uri;
    *out_source_t = me_rational_t{
        active->source_start.num * clip_local.den +
            clip_local.num * active->source_start.den,
        active->source_start.den * clip_local.den,
    };
    return ME_OK;
}

/* ------------------------------------------------------------ render */

me_status_t Player::render_one(const std::string&                            uri,
                                me_rational_t                                  source_t,
                                std::shared_ptr<me::graph::RgbaFrameData>*    out,
                                std::string*                                  err) const {
    if (!engine_ || !engine_->scheduler) return ME_E_INTERNAL;

    auto [g, term] = compile_frame_graph(uri, source_t);

    me::graph::EvalContext ctx;
    ctx.frames = engine_->frames.get();
    ctx.codecs = engine_->codecs.get();
    ctx.time   = source_t;

    try {
        auto fut = engine_->scheduler->evaluate_port<
                       std::shared_ptr<me::graph::RgbaFrameData>>(g, term, ctx);
        *out = fut.await();
    } catch (const std::exception& ex) {
        if (err) *err = ex.what();
        return ME_E_DECODE;
    }
    return *out ? ME_OK : ME_E_DECODE;
}

/* ---------------------------------------------------- producer thread */

void Player::producer_loop() {
    while (true) {
        me_rational_t cursor;
        {
            std::unique_lock<std::mutex> lk(mu_);
            /* Wait for: shutdown OR cursor inside the timeline (so we
             * have something to produce). When `produce_cursor_` is
             * past the timeline duration we sit here until a seek
             * brings it back. */
            state_cv_.wait(lk, [this] {
                if (shutdown_) return true;
                if (!tl_)      return false;
                /* Has clips left? We use simple cursor-vs-duration
                 * check; gaps inside the timeline are handled
                 * downstream by resolve_active_clip returning
                 * NOT_FOUND, in which case we advance + retry. */
                return r_cmp(produce_cursor_, tl_->duration) < 0;
            });
            if (shutdown_) return;
            cursor = produce_cursor_;
        }

        /* Compile + evaluate. resolve_active_clip + render_one are
         * both blocking; the slow piece is render_one (decode +
         * sws_scale). The scheduler's OutputCache absorbs repeats. */
        std::string  uri;
        me_rational_t source_t{0, 1};
        const me_status_t lookup = resolve_active_clip(cursor, &uri, &source_t);

        if (lookup == ME_E_NOT_FOUND) {
            /* Gap inside the timeline. Advance one frame and try
             * again — the timeline's clip layout will eventually
             * either cover the new cursor or push us past duration. */
            std::lock_guard<std::mutex> lk(mu_);
            if (r_cmp(produce_cursor_, cursor) == 0) {
                produce_cursor_ = r_add(produce_cursor_, frame_period_);
            }
            continue;
        }
        if (lookup != ME_OK) {
            /* Asset / timeline malformed — pause output. The host can
             * recover via seek + replace timeline at the C API. */
            std::unique_lock<std::mutex> lk(mu_);
            if (shutdown_) return;
            state_cv_.wait_for(lk, std::chrono::milliseconds(50));
            if (shutdown_) return;
            continue;
        }

        std::shared_ptr<me::graph::RgbaFrameData> rgba;
        std::string err;
        const me_status_t r = render_one(uri, source_t, &rgba, &err);
        if (r != ME_OK || !rgba) {
            if (engine_) me::detail::set_error(engine_, std::move(err));
            /* Skip this frame, advance, keep going. */
            std::lock_guard<std::mutex> lk(mu_);
            if (r_cmp(produce_cursor_, cursor) == 0) {
                produce_cursor_ = r_add(produce_cursor_, frame_period_);
            }
            continue;
        }

        VideoFrameSlot slot;
        slot.present_at = cursor;
        slot.rgba       = std::move(rgba);
        if (!ring_.push(std::move(slot))) {
            /* Ring closed — shutdown in flight. */
            return;
        }

        /* Advance only if no seek raced us. Otherwise the seek
         * already updated produce_cursor_ to the seek target. */
        std::lock_guard<std::mutex> lk(mu_);
        if (r_cmp(produce_cursor_, cursor) == 0) {
            produce_cursor_ = r_add(produce_cursor_, frame_period_);
        }
    }
}

/* ------------------------------------------------------ pacer thread */

void Player::pacer_loop() {
    while (true) {
        VideoFrameSlot slot;
        if (!ring_.pop(&slot)) {
            return;   /* closed */
        }

        /* Wait until the master clock reaches `slot.present_at`.
         * Polled in short intervals so a pause / seek wakes us via
         * state_cv_ within ≤ 5 ms. */
        while (true) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (shutdown_) return;
            }

            const me_rational_t now = clock_.current();
            const int64_t diff_us   = r_micros_diff(slot.present_at, now);

            /* Half-frame_period in micros. ½ × den/num seconds → in
             * micros = (den * 500'000) / num. */
            const int64_t half_us =
                frame_period_.num > 0
                    ? static_cast<int64_t>(frame_period_.den) * 500'000
                          / static_cast<int64_t>(frame_period_.num)
                    : 16'000;

            if (!clock_.is_playing()) {
                /* Park here until play()/seek() wakes us. The slot
                 * becomes stale across pauses; on resume we either
                 * present it (still close enough) or drop it (clock
                 * has jumped past via seek). Bare wait_for so any
                 * notify (play / pause / seek / shutdown) wakes the
                 * pacer immediately. */
                std::unique_lock<std::mutex> lk(mu_);
                if (shutdown_) return;
                state_cv_.wait_for(lk, std::chrono::milliseconds(20));
                if (shutdown_) return;
                continue;
            }

            if (diff_us > half_us) {
                /* Frame is too early. Sleep until close to present_at,
                 * but cap the wait so a pause / seek wakes us
                 * promptly via state_cv_. */
                std::unique_lock<std::mutex> lk(mu_);
                if (shutdown_) return;
                const auto budget = std::chrono::microseconds(
                    std::min<int64_t>(diff_us - half_us, 10'000));
                state_cv_.wait_for(lk, budget);
                if (shutdown_) return;
                continue;
            }
            if (diff_us < -half_us * 3) {
                /* Stale — produced before a seek that already
                 * advanced the clock past us, or pacer fell badly
                 * behind. Drop it; chase-audio in Phase 4 will
                 * formalise the threshold. */
                break;
            }
            /* Within half-period of `now` — present. */

            me_player_video_cb cb;
            void*              user;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb   = video_cb_;
                user = video_user_;
            }
            if (cb && slot.rgba) {
                me_frame frame;
                frame.width  = slot.rgba->width;
                frame.height = slot.rgba->height;
                frame.stride = static_cast<int>(slot.rgba->stride);
                /* Borrow-vs-copy: me_frame's rgba is a std::vector,
                 * not a pointer, so we copy bytes for the duration
                 * of the callback. ~3.7 MB at 720p; acceptable for
                 * Phase 2. A future optimisation can introduce a
                 * borrowed-buffer me_frame variant if scrubbing
                 * pressure shows up. */
                frame.rgba = slot.rgba->rgba;
                cb(&frame, slot.present_at, user);
            }
            break;
        }
    }
}

}  // namespace me::orchestrator
