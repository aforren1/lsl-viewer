#pragma once
// Replays the streams of an XDF file as live LSL outlets. Used by the xdf_replay
// CLI; the API is shaped so that a GUI can drive it too (pause, seek, speed, loop,
// and a status snapshot for a progress bar).
//
// Each stream becomes an outlet with the recorded name, type, channel count,
// nominal rate, channel format, and the <desc> element as recorded (channel
// labels, units, locations, ...). The source_id gets a prefix ("replay-" by
// default), so a recorder or a viewer that keys on it does not take the replay
// for the live device. All outlets share one playback clock, so the timing
// between streams is the same as in the recording. Clock sync and dejitter come
// from xdf_reader.hpp, so the timestamps are the ones pyxdf.load_xdf returns.
//
// Threads. prepare() runs on the caller's thread: the scan, the timestamp
// pre-pass that some streams need, and the outlets. play() starts two threads.
// The reader decodes the chunks of the selected streams in order of their start
// time and stays `lookahead` seconds of wall time ahead of playback. The player
// pushes each stream's samples when they are due: regular streams in chunks of
// about chunkMs, irregular streams at the time of each sample. Decoded chunks go
// through a pool per stream, so playback stops allocating once the pools have
// grown. The control calls and status() are safe from any thread.
//
// Timestamps. Playback follows a media position: recording seconds after the
// window start, plus one window length per loop pass. A sample is due when the
// playback clock reaches its position; that clock runs at `speed` while playing
// and stands still while paused. The pushed timestamp comes from a stamp clock
// that never stops or steps back:
//   - while playing, it advances by the recorded time (Stamps::recording) or by
//     the wall time (Stamps::scaled, the behavior of tools/xdf_replay.py);
//   - while paused, it advances by the wall time.
// A pause thus leaves a gap of its wall-clock length in the timestamps, as a
// device that stopped sending would. With holdStampsWhilePaused the stamp clock
// stops instead, so a pause leaves no gap: a viewer must not show a pause of the
// replay as data missing from the recording. A seek continues from the current
// stamp plus one sample period of the fastest stream: no gap, new content. A loop
// wrap continues with the next pass shifted by the window length. At 1x, both modes
// give timestamps that track local_clock(). At other speeds, recording stamps keep
// the recorded spacing (a viewer that plots by sample index and places markers by
// timestamp then shows correct times and dropout widths) and run ahead of
// local_clock(); scaled stamps keep tracking local_clock() but compress the
// recorded intervals.
//
// Breaks. Seeks, restarts from the end, and loop wraps join two parts of the
// recording that were not adjacent, and the stamps do not show it. breaks() lists
// each one with the stamp where the new part starts: every sample pushed before it
// has a lower stamp, and every sample after it has the same or a higher stamp. A
// viewer can mark the join from that.

#include "profiler.hpp"
#include "xdf_reader.hpp"

#include <lsl_c.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

class XdfPlayer {
public:
    enum class Stamps { recording, scaled };
    enum class State { idle, preparing, ready, playing, paused, finished, stopped, failed };

    struct Options {
        std::string streams;                 // selection, see xdf::selectStreams; empty: all
        std::string prefix, suffix;          // added to every outlet name
        std::string sourceIdPrefix = "replay-";
        double start = 0.0;                  // window start, seconds after the file's first sample
        std::optional<double> duration;      // window length; none: to the end
        double speed = 1.0;
        bool   loop = false;
        double delay = 0.5;                  // seconds from play() to the first sample
        double chunkMs = 20.0;               // push period of regular streams
        double lookahead = 2.0;              // wall seconds the reader decodes ahead
        bool   sync = true;                  // pyxdf clock synchronization
        bool   dejitter = false;             // pyxdf dejitter (reads every timestamp first)
        Stamps stamps = Stamps::recording;
        bool   holdStampsWhilePaused = false; // the stamp clock stops during a pause (no gap)
        xdf::WarnFn onWarning;               // called for each warning, from any thread
    };

    struct Break {
        enum class Kind : std::uint8_t { seek, restart, loop };
        Kind   kind = Kind::seek;
        double stamp = 0.0;                  // pushed timestamp where the new part starts
        double position = 0.0;               // seconds into the window where it starts
    };

    struct StreamStatus {
        std::string   name, type, format;    // name as the outlet has it
        int           channels = 0;
        double        srate = 0.0;           // 0: irregular
        std::uint64_t fileSamples = 0;       // in the whole file, not only the window
        std::uint64_t pushed = 0;
    };
    struct Status {
        State         state = State::idle;
        double        position = 0.0;        // seconds into the window, in the current pass
        double        length = 0.0;          // window length in seconds
        bool          lengthExact = false;   // estimated from the index until a pass completes
        double        speed = 1.0;
        bool          loop = false;
        std::uint64_t pass = 0;              // loop passes completed since the last seek
        std::uint64_t late = 0;              // chunks decoded after their time (pushed at once)
        std::size_t   warnings = 0;          // count; see warnings()
        std::size_t   breaks = 0;            // count; see breaks()
        bool          empty = false;         // the window holds no samples
        std::vector<StreamStatus> streams;
    };

    XdfPlayer() = default;
    XdfPlayer(const XdfPlayer&) = delete;
    XdfPlayer& operator=(const XdfPlayer&) = delete;
    ~XdfPlayer() { stop(); }

    // Scans the file, applies clock sync (and dejitter), and creates the outlets.
    // Blocks for the scan and pre-pass (seconds on multi-GB files); cancel() from
    // another thread aborts it. False on error, see error().
    bool prepare(const std::filesystem::path& file, const Options& opt);
    // Starts playback (after opt.delay). False unless prepare() succeeded.
    bool play();

    void pause();
    // Continues after pause(); after the end, starts again from the window start.
    void resume();
    // Jumps to `seconds` after the window start (clamped to the window). Keeps the
    // play or pause state; from the end, it pauses there.
    void seek(double seconds);
    void setSpeed(double speed);
    // Applies at the end of the current pass, until the last chunk has been pushed.
    void setLoop(bool loop);
    // Stops playback, joins the threads, and closes the outlets. Idempotent.
    void stop();
    // Async-signal-safe request to abort prepare(); stop() still has to follow.
    void cancel() noexcept { cancel_.store(true, std::memory_order_relaxed); }

    void        status(Status& out) const;   // reuses out's buffers
    Status      status() const { Status s; status(s); return s; }
    std::vector<std::string> warnings(std::size_t from = 0) const;
    // Appends breaks number `from` onward to `out` and returns the total count. Only the
    // newest kMaxBreaks are kept, so a caller far behind skips the oldest.
    std::size_t breaks(std::vector<Break>& out, std::size_t from = 0) const;
    std::string error() const { std::lock_guard<std::mutex> lk(m_); return error_; }
    // Waits until playback finished (or stopped). False on timeout.
    bool waitFinished(double seconds) const;

    // For a report of the preparation (the CLI prints these).
    const std::vector<xdf::StreamHeader>& headers() const { return headers_; }
    const std::vector<std::size_t>&       selected() const { return selected_; }
    std::size_t prepassStreams() const { return prepassStreams_; }
    double      prepassSeconds() const { return prepassSeconds_; }
    // The liblsl uid of each outlet, in the order of selected(). A resolve by uid finds these
    // outlets and not a replay of the same file on another host. Valid until stop().
    std::vector<std::string> outletUids() const;

private:
    using clock = std::chrono::steady_clock;
    static constexpr double kInf = std::numeric_limits<double>::infinity();
    static constexpr double kSeekLead = 0.1;   // wall seconds, see restartAt()
    // A loop of a short window adds a break per pass for as long as it runs.
    static constexpr std::size_t kMaxBreaks = 4096;

    struct Item {                              // one decoded chunk
        std::vector<std::uint8_t> raw;         // the chunk bytes; string samples point into them
        xdf::Samples              s;
        std::vector<double>       media;       // media position of each sample
        std::vector<double>       mono;        // the same made non-decreasing, for scheduling
        std::size_t               next = 0;    // first sample not pushed yet
    };
    // FIFO of decoded chunks. A vector with a moving head: the read-ahead keeps it
    // from ever running empty during playback, and std::deque allocates a block every
    // few elements on MSVC.
    struct Fifo {
        std::vector<Item*> v;
        std::size_t        head = 0;
        bool  empty() const { return head == v.size(); }
        void  push(Item* it) { v.push_back(it); }
        Item* pop() {
            Item* it = v[head++];
            if (head == v.size()) { v.clear(); head = 0; }
            else if (head >= 64 && head * 2 >= v.size()) {   // drop the consumed front, keep the capacity
                v.erase(v.begin(), v.begin() + (std::ptrdiff_t)head);
                head = 0;
            }
            return it;
        }
        void drainTo(std::vector<Item*>& pool) {
            for (std::size_t i = head; i < v.size(); ++i) pool.push_back(v[i]);
            v.clear();
            head = 0;
        }
    };
    struct Track {
        std::size_t         hdr = 0;           // into headers_
        lsl_outlet          outlet = nullptr;
        std::string         outName;
        bool                regular = false;
        std::uint64_t       limit = 0;         // samples to use (pyxdf's pylsl#67 truncation)
        std::vector<double> cref;              // chunk starts before dejitter, to check stamps
        std::vector<double> ref;               // chunk starts as replayed, to pace and seek
        xdf::StartPlan      plan0;             // where a pass from the window start begins
        // Shared between the threads, under m_.
        Fifo                               queue;
        Item*                              cur = nullptr;
        std::vector<std::unique_ptr<Item>> items;
        std::vector<Item*>                 pool;
        double                             nextPush = -kInf;
        std::uint64_t                      pushed = 0;
        bool                               pushFailed = false;
        // Player thread only.
        std::vector<double>                stamps;
    };

    // --- clock, under m_ ----------------------------------------------------
    double mediaAt(double now) const {
        return state_ == State::playing ? mediaA_ + (now - wallA_) * speed_ : mediaA_;
    }
    double dueOf(double media) const { return wallA_ + (media - mediaA_) / speed_; }
    double stampOf(double media) const {
        const double dm = media - mediaA_;
        return stampA_ + (stamps_ == Stamps::recording ? dm : dm / speed_);
    }
    // The stamp clock. Before wallA_ (the start delay, a seek's lead time) it holds at
    // stampA_, the stamp of the first sample to come, so a rebase there cannot move it back.
    double stampClock(double now) const {
        if (state_ != State::playing) return hold_ ? stampA_ : stampA_ + (now - wallA_);
        return now < wallA_ ? stampA_ : stampOf(mediaAt(now));
    }
    void addBreak(Break::Kind kind, double stamp, double position) {
        if (breaks_.size() >= kMaxBreaks) {
            const std::size_t drop = kMaxBreaks / 2;
            breaks_.erase(breaks_.begin(), breaks_.begin() + (std::ptrdiff_t)drop);
            breaksBase_ += drop;
        }
        breaks_.push_back({kind, stamp, position});
    }
    // Called with the media position of each sample pushed. The first sample of a later
    // pass marks the wrap; detecting it on a pushed sample (not on the clock) keeps a
    // late player thread at the end of a non-looping window from reporting a wrap.
    void noteWrap(double media) {
        if (!loopKnown_ || !(loopLen_ > 0)) return;
        const double k = std::floor(media / loopLen_);
        if (k <= (double)wrapK_) return;
        wrapK_ = (std::uint64_t)k;
        addBreak(Break::Kind::loop, stampOf(k * loopLen_), 0.0);
    }
    // Moves the anchors to now without changing the clocks.
    void rebase(double now) {
        const double c = stampClock(now);
        mediaA_ = mediaAt(now);
        stampA_ = c;
        wallA_ = now;
    }
    double lengthLocked() const {
        if (loopKnown_) return loopLen_ - gap_;
        return std::isfinite(hi_) ? std::min(hi_ - lo_, lengthEstimate_) : lengthEstimate_;
    }
    void wakeAll() {
        for (Track& t : tracks_) t.nextPush = -kInf;
        playerCv_.notify_all();
        readerCv_.notify_all();
    }
    void flushLocked() {
        for (Track& t : tracks_) {
            t.queue.drainTo(t.pool);
            if (t.cur) { t.pool.push_back(t.cur); t.cur = nullptr; }
        }
    }
    // Moves playback to media position p (a seek, or a restart from the end). Every
    // sample pushed so far was due by now, so its stamp is at most the stamp clock's
    // value; one fastest-stream period on top keeps the next stamp later. While
    // playing, the new position is due kSeekLead from now, which gives the reader
    // time to decode it, so its first chunks are not late. The stamps do not count
    // the lead: the sample at p gets the stamp clock's value plus the period.
    void restartAt(double now, double p, bool playing, Break::Kind kind) {
        const double lead = playing ? kSeekLead : 0.0;
        stampA_ = stampClock(now) + gap_;      // the clock of the state before the call
        mediaA_ = p;
        wallA_ = now + lead;
        flushLocked();
        ++gen_;
        seekTo_ = p;
        ended_ = false;
        wrapK_ = 0;                            // media restarts below one window length
        addBreak(kind, stampA_, p);
    }
    Item* acquire(Track& t) {
        if (t.pool.empty()) { t.items.push_back(std::make_unique<Item>()); return t.items.back().get(); }
        Item* it = t.pool.back();
        t.pool.pop_back();
        return it;
    }

    void warn(const std::string& msg) {
        { std::lock_guard<std::mutex> lk(wm_); warnings_.push_back(msg); }
        if (opt_.onWarning) opt_.onWarning(msg);
    }
    void fail(const std::string& msg) {
        std::lock_guard<std::mutex> lk(m_);
        error_ = msg;
        state_ = State::failed;
    }
    void closeOutlets() {
        for (Track& t : tracks_) if (t.outlet) { lsl_destroy_outlet(t.outlet); t.outlet = nullptr; }
    }
    lsl_outlet makeOutlet(const xdf::StreamHeader& h, const std::string& name, std::string& err) const;

    void readerMain();
    // One pass over the window from `from` seconds after its start. Returns false if
    // a seek or stop cut it short; endTs gets the latest timestamp it delivered.
    bool runPass(std::unique_lock<std::mutex>& lk, xdf::InFile& f, std::uint64_t gen, double from,
                 std::uint64_t k, double& endTs);
    void playerMain();
    // Pushes every sample due by now. throttle: regular streams wait out their push
    // period (the player loop); a control call that moves the clocks pushes all that is
    // due first, so those samples keep their stamps from the old clock. Lowers wake to
    // the next push time; returns true if nothing is queued or in progress.
    bool pushDue(double now, bool throttle, double& wake);
    void pushRange(Track& t, Item& it, std::size_t i, std::size_t j);

    // Set by prepare(), read-only afterwards.
    Options                        opt_;
    std::filesystem::path          path_;
    std::vector<xdf::StreamHeader> headers_;
    std::vector<xdf::Timebase>     bases_;
    std::vector<std::size_t>       selected_;
    std::vector<Track>             tracks_;
    double lo_ = 0.0, hi_ = kInf, gap_ = 1e-3, lengthEstimate_ = 0.0;
    std::size_t prepassStreams_ = 0;
    double      prepassSeconds_ = 0.0;

    // Shared state, under m_.
    mutable std::mutex              m_;
    std::condition_variable         readerCv_, playerCv_;
    mutable std::condition_variable doneCv_;
    State         state_ = State::idle;
    Stamps        stamps_ = Stamps::recording;
    double        wallA_ = 0.0, mediaA_ = 0.0, stampA_ = 0.0, speed_ = 1.0;
    bool          loop_ = false, stop_ = false;
    std::uint64_t gen_ = 0;                  // bumped by each seek
    double        seekTo_ = 0.0;
    bool          ended_ = false;            // the reader delivered everything for this gen
    bool          empty_ = false;
    bool          loopKnown_ = false;
    double        loopLen_ = 0.0;            // recording seconds per loop pass
    std::uint64_t late_ = 0;
    std::string   error_;
    bool          hold_ = false;             // Options::holdStampsWhilePaused
    std::uint64_t wrapK_ = 0;                // loop passes since the last seek with a break
    std::vector<Break> breaks_;
    std::size_t   breaksBase_ = 0;           // breaks dropped from the front of breaks_

    mutable std::mutex       wm_;
    std::vector<std::string> warnings_;
    std::unordered_set<std::uint64_t> warnedChunks_;   // reader thread only

    std::atomic<bool> cancel_{false};
    std::thread       reader_, player_;
};

// ---------------------------------------------------------------------------
// Preparation
// ---------------------------------------------------------------------------
namespace xdf_player_detail {

inline std::string escapeXml(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

// Round-trip text of a double, independent of the C locale (a GUI may set one with
// a decimal comma).
inline std::string shortest(double v) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o.precision(17);
    o << v;
    return o.str();
}

}  // namespace xdf_player_detail

inline lsl_outlet XdfPlayer::makeOutlet(const xdf::StreamHeader& h, const std::string& name, std::string& err) const {
    using xdf_player_detail::escapeXml;
    // Building the info from XML keeps the recorded <desc> as it is, including element
    // order and attributes. The skeleton lists every field liblsl expects: it rejects
    // a header with a missing number or uid, and the outlet can fill in its live
    // session_id, hostname, and ports only where the elements already exist.
    const std::string sid = opt_.sourceIdPrefix + (h.sourceId.empty() ? h.name : h.sourceId);
    std::string xml;
    xml.reserve(512 + h.desc.size());
    xml += "<?xml version=\"1.0\"?><info><name>";
    xml += escapeXml(name);
    xml += "</name><type>" + escapeXml(h.type) + "</type><channel_count>" + std::to_string(h.channels) +
           "</channel_count><channel_format>" + h.formatName + "</channel_format><source_id>" + escapeXml(sid) +
           "</source_id><nominal_srate>" + xdf_player_detail::shortest(h.srate) +
           "</nominal_srate><version>1.1</version><created_at>0</created_at><uid>replay</uid>"
           "<session_id>default</session_id><hostname>replay</hostname><v4address></v4address>"
           "<v4data_port>0</v4data_port><v4service_port>0</v4service_port><v6address></v6address>"
           "<v6data_port>0</v6data_port><v6service_port>0</v6service_port>";
    xml += h.desc.empty() ? std::string("<desc />") : h.desc;
    xml += "</info>";
    lsl_streaminfo info = lsl_streaminfo_from_xml(xml.c_str());
    if (!info || lsl_get_channel_count(info) != h.channels) {
        if (info) lsl_destroy_streaminfo(info);
        err = "liblsl rejected the header of stream " + std::to_string(h.id) + ": " + name;
        return nullptr;
    }
    // A chunk_size hint lets liblsl batch regular data the same way the push loop does.
    const int chunk = h.srate > 0 ? std::max(1, (int)std::nearbyint(h.srate * (opt_.chunkMs / 1000.0))) : 0;
    lsl_outlet o = lsl_create_outlet(info, chunk, 360);
    lsl_destroy_streaminfo(info);
    if (!o) err = "liblsl could not create the outlet for " + name;
    return o;
}

inline bool XdfPlayer::prepare(const std::filesystem::path& file, const Options& opt) {
    stop();
    {
        std::lock_guard<std::mutex> lk(m_);
        state_ = State::preparing;
        error_.clear();
        stop_ = false;
        gen_ = 0; seekTo_ = 0.0; ended_ = empty_ = loopKnown_ = false; loopLen_ = 0.0; late_ = 0;
        stamps_ = opt.stamps;
        speed_ = opt.speed > 0 ? opt.speed : 1.0;
        loop_ = opt.loop;
        hold_ = opt.holdStampsWhilePaused;
        wrapK_ = 0;
        breaks_.clear();
        breaksBase_ = 0;
    }
    { std::lock_guard<std::mutex> lk(wm_); warnings_.clear(); }
    warnedChunks_.clear();
    cancel_.store(false);
    opt_ = opt;
    path_ = file;
    tracks_.clear();
    headers_.clear();
    bases_.clear();
    selected_.clear();
    prepassStreams_ = 0;
    prepassSeconds_ = 0.0;
    const xdf::WarnFn warnFn = [this](const std::string& m) { warn(m); };
    try {
        headers_ = xdf::scan(file, warnFn, &cancel_);
        for (const auto& h : headers_) bases_.emplace_back(h, opt.sync, warnFn);
        std::string err;
        selected_ = xdf::selectStreams(headers_, opt.streams, err);
        if (!err.empty()) { fail(err); return false; }

        const auto t = clock::now();
        for (std::size_t k : selected_) {
            const auto& h = headers_[k];
            auto& tb = bases_[k];
            if (!tb.needsPrepass() && !(opt.dejitter && h.srate > 0 && h.count)) continue;
            std::vector<double> raw = xdf::readAllStamps(file, h, tb, &cancel_);
            if (tb.needsPrepass()) tb.assignRanges(raw);
            if (opt.dejitter) {
                tb.clock(raw.data(), 0, raw.size());
                tb.fitDejitter(raw, h);
            }
            ++prepassStreams_;
        }
        prepassSeconds_ = std::chrono::duration<double>(clock::now() - t).count();

        // Time zero is the first sample in the file, also when it belongs to a stream
        // that is not replayed, so `start` means the same as the --list column.
        // Dejitter can move a replayed stream's first sample earlier; time zero
        // follows it so that start 0 keeps every sample.
        double t0 = kInf;
        for (std::size_t k = 0; k < headers_.size(); ++k)
            if (auto f = xdf::firstTime(headers_[k], bases_[k])) t0 = std::min(t0, *f);
        if (!std::isfinite(t0)) { fail("the file contains no samples"); return false; }
        lo_ = t0 + opt.start;
        hi_ = opt.duration ? lo_ + *opt.duration : kInf;

        double maxRate = 0.0;
        lengthEstimate_ = 0.0;
        tracks_.resize(selected_.size());
        for (std::size_t i = 0; i < selected_.size(); ++i) {
            if (cancel_.load()) throw std::runtime_error("cancelled");
            Track& tr = tracks_[i];
            const auto& h = headers_[selected_[i]];
            const auto& tb = bases_[selected_[i]];
            tr.hdr = selected_[i];
            tr.outName = opt.prefix + h.name + opt.suffix;
            tr.regular = h.srate > 0;
            tr.limit = tb.limit ? *tb.limit : h.count;
            tr.outlet = makeOutlet(h, tr.outName, err);
            if (!tr.outlet) { closeOutlets(); fail(err); return false; }
            maxRate = std::max(maxRate, h.srate);
            if (h.chunks.empty()) continue;
            tr.cref = xdf::chunkStarts(xdf::clockedRefs(h, tb), h.chunks, h.tdiff);
            if (tb.dejittered()) {
                tr.ref.resize(h.chunks.size());
                for (std::size_t c = 0; c < h.chunks.size(); ++c) tr.ref[c] = tb.final(h.chunks[c].ref, h.chunks[c].g);
            } else {
                tr.ref = tr.cref;
            }
            tr.plan0 = xdf::planStart(h, tr.ref, lo_);
            if (h.last && h.count) lengthEstimate_ = std::max(lengthEstimate_, tb.final(*h.last, h.count - 1) - lo_);
        }
        // A gap of one period of the fastest stream keeps a loop wrap seamless for a
        // stream that spans the window and keeps every stream strictly increasing.
        gap_ = maxRate > 0 ? 1.0 / maxRate : 1e-3;
    } catch (const std::exception& e) {
        closeOutlets();
        fail(e.what());
        return false;
    }
    std::lock_guard<std::mutex> lk(m_);
    state_ = State::ready;
    return true;
}

inline std::vector<std::string> XdfPlayer::outletUids() const {
    std::vector<std::string> out;
    out.reserve(tracks_.size());
    for (const Track& t : tracks_) {
        std::string uid;
        if (t.outlet)
            if (lsl_streaminfo info = lsl_get_info(t.outlet)) {
                if (const char* u = lsl_get_uid(info)) uid = u;
                lsl_destroy_streaminfo(info);
            }
        out.push_back(std::move(uid));
    }
    return out;
}

inline bool XdfPlayer::play() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (state_ != State::ready) return false;
        const double base = lsl_local_clock() + opt_.delay;
        wallA_ = base;
        mediaA_ = 0.0;
        stampA_ = base;
        state_ = State::playing;
    }
    reader_ = std::thread([this] { readerMain(); });
    player_ = std::thread([this] { playerMain(); });
    return true;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------
inline void XdfPlayer::pause() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ != State::playing) return;
    const double now = lsl_local_clock();
    double wake = 0;
    pushDue(now, false, wake);
    rebase(now);
    state_ = State::paused;
    wakeAll();
}

inline void XdfPlayer::resume() {
    std::lock_guard<std::mutex> lk(m_);
    const double now = lsl_local_clock();
    if (state_ == State::finished) {
        // From the end: start the window again, like a media player.
        restartAt(now, 0.0, true, Break::Kind::restart);
        state_ = State::playing;
    } else if (state_ == State::paused) {
        stampA_ = stampClock(now);   // the stamp clock ran on at wall speed during the pause
        wallA_ = now;
        state_ = State::playing;
    } else {
        return;
    }
    wakeAll();
}

inline void XdfPlayer::seek(double seconds) {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ != State::playing && state_ != State::paused && state_ != State::finished) return;
    const double now = lsl_local_clock();
    const double p = std::clamp(seconds, 0.0, std::max(0.0, lengthLocked()));
    if (state_ == State::playing) {
        // The flush drops queued samples, and those already due would leave a hole of up
        // to one push period (times the speed) in the stamps before the seek.
        double wake = 0;
        pushDue(now, false, wake);
    }
    restartAt(now, p, state_ == State::playing, Break::Kind::seek);
    if (state_ == State::finished) state_ = State::paused;
    wakeAll();
}

inline void XdfPlayer::setSpeed(double speed) {
    if (!(speed > 0) || !std::isfinite(speed)) return;
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == State::playing) {
        // Scaled stamps change slope with the speed; samples already due must keep the
        // old slope, or they would be stamped before the ones pushed last.
        const double now = lsl_local_clock();
        double wake = 0;
        pushDue(now, false, wake);
        rebase(now);
    }
    speed_ = speed;
    wakeAll();
}

inline void XdfPlayer::setLoop(bool loop) {
    std::lock_guard<std::mutex> lk(m_);
    loop_ = loop;
    wakeAll();
}

inline void XdfPlayer::stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
        if (state_ != State::idle && state_ != State::failed) state_ = State::stopped;
        readerCv_.notify_all();
        playerCv_.notify_all();
        doneCv_.notify_all();
    }
    cancel_.store(true);
    if (reader_.joinable()) reader_.join();
    if (player_.joinable()) player_.join();
    closeOutlets();
}

inline bool XdfPlayer::waitFinished(double seconds) const {
    std::unique_lock<std::mutex> lk(m_);
    return doneCv_.wait_for(lk, std::chrono::duration<double>(seconds), [this] {
        return state_ == State::finished || state_ == State::stopped || state_ == State::failed;
    });
}

inline void XdfPlayer::status(Status& out) const {
    std::lock_guard<std::mutex> lk(m_);
    const double now = lsl_local_clock();
    out.state = state_;
    out.speed = speed_;
    out.loop = loop_;
    out.late = late_;
    out.empty = empty_;
    out.length = std::max(0.0, lengthLocked());
    out.lengthExact = loopKnown_ || std::isfinite(hi_);
    // During a seek's lead time the clock is still short of the target; show the target.
    const double m = std::max(0.0, state_ == State::playing && now < wallA_ ? mediaA_ : mediaAt(now));
    out.pass = 0;
    if (state_ == State::finished) out.position = out.length;
    else if (loopKnown_ && loopLen_ > 0) {
        const double k = std::floor(m / loopLen_);
        out.pass = (std::uint64_t)k;
        out.position = std::min(m - k * loopLen_, out.length);
    } else out.position = std::min(m, out.length);
    out.streams.resize(tracks_.size());
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const auto& h = headers_[tracks_[i].hdr];
        StreamStatus& s = out.streams[i];
        s.name = tracks_[i].outName;
        s.type = h.type;
        s.format = h.formatName;
        s.channels = h.channels;
        s.srate = h.srate;
        s.fileSamples = h.count;
        s.pushed = tracks_[i].pushed;
    }
    out.breaks = breaksBase_ + breaks_.size();
    std::lock_guard<std::mutex> wl(wm_);
    out.warnings = warnings_.size();
}

inline std::vector<std::string> XdfPlayer::warnings(std::size_t from) const {
    std::lock_guard<std::mutex> lk(wm_);
    if (from >= warnings_.size()) return {};
    return std::vector<std::string>(warnings_.begin() + (std::ptrdiff_t)from, warnings_.end());
}

inline std::size_t XdfPlayer::breaks(std::vector<Break>& out, std::size_t from) const {
    std::lock_guard<std::mutex> lk(m_);
    const std::size_t total = breaksBase_ + breaks_.size();
    for (std::size_t i = std::max(from, breaksBase_); i < total; ++i) out.push_back(breaks_[i - breaksBase_]);
    return total;
}

// ---------------------------------------------------------------------------
// Reader thread
// ---------------------------------------------------------------------------
inline void XdfPlayer::readerMain() {
    xdf::InFile f;
    if (!f.open(path_)) {
        warn(path_.string() + ": cannot open the file again, replay stops");
        std::lock_guard<std::mutex> lk(m_);
        ended_ = true;
        playerCv_.notify_all();
        return;
    }
    std::unique_lock<std::mutex> lk(m_);
    std::uint64_t gen = gen_, k = 0;
    double from = 0.0;
    bool first = true;
    while (!stop_) {
        double endTs = -kInf;
        bool complete = false, broken = false;
        try {
            complete = runPass(lk, f, gen, from, k, endTs);
        } catch (const std::exception& e) {
            if (lk.owns_lock()) lk.unlock();
            warn(std::string("damaged chunk (") + e.what() + "), replay stops there");
            lk.lock();
            complete = broken = true;
        }
        if (stop_) return;
        if (complete && gen == gen_) {
            if (!loopKnown_ && !broken) {
                if (endTs == -kInf) empty_ = first;
                else { loopLen_ = endTs - lo_ + gap_; loopKnown_ = true; }
            }
            first = false;
            const auto canLoop = [&] { return !broken && loop_ && loopKnown_; };
            if (canLoop()) { ++k; from = 0.0; continue; }
            ended_ = true;
            playerCv_.notify_all();
            readerCv_.wait(lk, [&] { return stop_ || gen != gen_ || (canLoop() && state_ != State::finished); });
            if (stop_) return;
            if (gen == gen_) {           // looping was turned on before the end
                ended_ = false;
                ++k; from = 0.0;
                continue;
            }
        }
        // A seek: start over from its position, as the first pass.
        gen = gen_;
        from = seekTo_;
        k = 0;
        first = false;
    }
}

inline bool XdfPlayer::runPass(std::unique_lock<std::mutex>& lk, xdf::InFile& f, std::uint64_t gen,
                               double from, std::uint64_t k, double& endTs) {
    const double lo = lo_ + from;           // samples before this are skipped
    const double L = k ? loopLen_ : 0.0;
    const std::size_t nt = tracks_.size();
    std::vector<std::size_t> cursor(nt);
    std::vector<double>      last(nt);
    // Plain files are read in order of chunk start time, so a stream whose next chunk
    // lies far ahead (a sparse marker stream, or another host's clock without sync)
    // cannot hold up the others. Ties go to the earlier stream.
    using Key = std::pair<double, std::size_t>;
    std::vector<Key> heap;
    auto keyOf = [&](std::size_t i, std::size_t c, double prev) {
        const double r = tracks_[i].ref[c];
        return std::isfinite(r) && r > prev ? r : prev;
    };
    for (std::size_t i = 0; i < nt; ++i) {
        const Track& t = tracks_[i];
        const auto& h = headers_[t.hdr];
        if (h.chunks.empty()) continue;
        const xdf::StartPlan p = from == 0.0 ? t.plan0 : xdf::planStart(h, t.ref, lo);
        cursor[i] = p.chunk;
        last[i] = p.last0;
        heap.push_back({keyOf(i, p.chunk, -kInf), i});
    }
    std::make_heap(heap.begin(), heap.end(), std::greater<Key>());
    std::vector<double> tmp;

    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), std::greater<Key>());
        const auto [key, i] = heap.back();
        heap.pop_back();
        Track& t = tracks_[i];
        const auto& h = headers_[t.hdr];
        const auto& tb = bases_[t.hdr];
        const std::size_t c = cursor[i];
        const xdf::ChunkRef& ch = h.chunks[c];
        bool done = true;                     // drop this stream from the pass
        const double ref = t.ref[c];
        if (ch.g < t.limit && !(std::isfinite(ref) && ref > hi_ + xdf::kMargin)) {
            if (std::isfinite(ref)) {
                // Stay `lookahead` seconds of wall time ahead of playback.
                const double mref = ref - lo_ + (double)k * L;
                for (;;) {
                    if (stop_ || gen != gen_) return false;
                    const double ahead = mref - mediaAt(lsl_local_clock()) - opt_.lookahead * speed_;
                    if (ahead <= 0) break;
                    const double wait = state_ == State::playing ? std::min(0.25, ahead / speed_) : 0.25;
                    readerCv_.wait_for(lk, std::chrono::duration<double>(wait));
                }
            }
            Item* it = acquire(t);
            lk.unlock();
            bool ok = true;
            std::string why;
            double newLast = last[i];
            {
                LSL_ZONE("xdf read+decode");
                try {
                    if (!f.readAt(ch.off, (std::size_t)(ch.end - ch.off), it->raw)) throw xdf::Damaged("cut-off chunk");
                    xdf::decode(h, it->raw.data(), it->raw.size(), newLast, true, it->s);
                    tmp.assign(it->s.ts.begin(), it->s.ts.begin() + (std::ptrdiff_t)it->s.n);
                    tb.clock(tmp.data(), ch.g, tmp.size());
                    if (!xdf::plausible(tmp.data(), tmp.size(), t.cref, c))
                        throw xdf::Damaged("timestamps do not fit the chunks around it");
                } catch (const xdf::Damaged& e) {
                    ok = false;
                    why = e.what();
                }
            }
            std::size_t n = 0;
            double tmin = kInf, tmax = -kInf;
            if (ok) {
                // The chunk length was intact (the scan indexed it) but the body may not
                // be; on failure the rest of the stream is still good.
                last[i] = newLast;
                xdf::Samples& s = it->s;
                n = (std::size_t)std::min<std::uint64_t>(s.n, t.limit - ch.g);
                tb.final(s.ts.data(), ch.g, n);
                bool any = false;                     // as numpy's nanmin: NaN stamps do not count
                for (std::size_t x = 0; x < n; ++x)
                    if (s.ts[x] == s.ts[x]) { any = true; tmin = std::min(tmin, s.ts[x]); }
                done = any && tmin > hi_ + xdf::kMargin;
                // Keep the samples in the window, compacting in place.
                const std::size_t nch = (std::size_t)h.channels, vs = h.valueSize;
                std::size_t w = 0;
                for (std::size_t x = 0; x < n; ++x) {
                    const double ts = s.ts[x];
                    if (!(ts >= lo && ts <= hi_)) continue;
                    if (w != x) {
                        s.ts[w] = ts;
                        if (vs) std::memmove(s.values.data() + w * vs, s.values.data() + x * vs, vs);
                        else for (std::size_t q = 0; q < nch; ++q) {
                            s.str[w * nch + q] = s.str[x * nch + q];
                            s.len[w * nch + q] = s.len[x * nch + q];
                        }
                    }
                    tmax = std::max(tmax, ts);
                    ++w;
                }
                s.n = n = w;
                it->media.resize(n);
                it->mono.resize(n);
                double run = -kInf;
                for (std::size_t x = 0; x < n; ++x) {
                    double m = s.ts[x] - lo_;
                    if (k) m += (double)k * L;
                    it->media[x] = m;
                    run = std::max(run, m);
                    it->mono[x] = run;
                }
                it->next = 0;
            } else if (k == 0 && warnedChunks_.insert(ch.off).second) {
                warn(h.name + ": chunk at byte " + std::to_string(ch.off) + " cannot be decoded (" + why +
                     "), skipped");
            }
            lk.lock();
            if (stop_ || gen != gen_) { t.pool.push_back(it); return false; }
            if (!ok) done = false;
            if (n == 0) {
                t.pool.push_back(it);
            } else {
                if (state_ == State::playing && dueOf(it->mono[0]) < lsl_local_clock()) ++late_;
                t.queue.push(it);
                endTs = std::max(endTs, tmax);
                playerCv_.notify_all();
            }
        }
        if (!done && c + 1 < h.chunks.size()) {
            cursor[i] = c + 1;
            heap.push_back({keyOf(i, c + 1, key), i});
            std::push_heap(heap.begin(), heap.end(), std::greater<Key>());
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Player thread
// ---------------------------------------------------------------------------
inline void XdfPlayer::pushRange(Track& t, Item& it, std::size_t i, std::size_t j) {
    LSL_ZONE("xdf push");
    const auto& h = headers_[t.hdr];
    const std::size_t m = j - i, nch = (std::size_t)h.channels;
    if (t.stamps.size() < m) t.stamps.resize(m);   // grows to the largest push, then stays
    for (std::size_t x = 0; x < m; ++x) t.stamps[x] = stampOf(it.media[i + x]);
    const auto elems = (unsigned long)(m * nch);
    const double* ts = t.stamps.data();
    const std::uint8_t* v = it.s.values.data() + i * h.valueSize;
    int rc = 0;
    if (elems) {
        switch (h.format) {
        case xdf::Format::float32:  rc = lsl_push_chunk_ftnp(t.outlet, reinterpret_cast<const float*>(v), elems, ts, 1); break;
        case xdf::Format::double64: rc = lsl_push_chunk_dtnp(t.outlet, reinterpret_cast<const double*>(v), elems, ts, 1); break;
        case xdf::Format::int8:     rc = lsl_push_chunk_ctnp(t.outlet, reinterpret_cast<const char*>(v), elems, ts, 1); break;
        case xdf::Format::int16:    rc = lsl_push_chunk_stnp(t.outlet, reinterpret_cast<const std::int16_t*>(v), elems, ts, 1); break;
        case xdf::Format::int32:    rc = lsl_push_chunk_itnp(t.outlet, reinterpret_cast<const std::int32_t*>(v), elems, ts, 1); break;
        case xdf::Format::int64:    rc = lsl_push_chunk_ltnp(t.outlet, reinterpret_cast<const std::int64_t*>(v), elems, ts, 1); break;
        case xdf::Format::string:
            rc = lsl_push_chunk_buftnp(t.outlet, it.s.str.data() + i * nch, it.s.len.data() + i * nch, elems, ts, 1);
            break;
        }
    }
    if (rc < 0 && !t.pushFailed) {
        t.pushFailed = true;
        warn(t.outName + ": liblsl push_chunk failed with error " + std::to_string(rc));
    }
    t.pushed += m;
}

inline bool XdfPlayer::pushDue(double now, bool throttle, double& wake) {
    const double M = mediaAt(now);
    bool idle = true;                        // nothing queued or in progress
    for (Track& t : tracks_) {
        if (throttle && t.regular && now < t.nextPush) { wake = std::min(wake, t.nextPush); idle = false; continue; }
        for (;;) {
            if (!t.cur) {
                if (t.queue.empty()) break;
                t.cur = t.queue.pop();
            }
            Item& it = *t.cur;
            const double* mono = it.mono.data();
            const auto j = (std::size_t)(std::upper_bound(mono + it.next, mono + it.s.n, M) - mono);
            if (j > it.next) {
                pushRange(t, it, it.next, j);
                noteWrap(it.media[j - 1]);
                it.next = j;
            }
            if (it.next >= it.s.n) {
                // File chunks can be shorter than one push period, so go straight on
                // to the next queued chunk instead of waiting.
                t.pool.push_back(t.cur);
                t.cur = nullptr;
                continue;
            }
            // Regular streams push about once per period; irregular streams wake up
            // at the time of their next sample.
            const double due = dueOf(mono[it.next]);
            t.nextPush = t.regular ? std::max(due, now + opt_.chunkMs / 1000.0) : due;
            break;
        }
        if (t.cur) { idle = false; wake = std::min(wake, t.nextPush); }
        else t.nextPush = -kInf;             // the next chunk goes out as soon as it arrives
    }
    return idle;
}

inline void XdfPlayer::playerMain() {
    std::unique_lock<std::mutex> lk(m_);
    while (!stop_) {
        if (state_ != State::playing) { playerCv_.wait(lk); continue; }
        const double now = lsl_local_clock();
        double wake = now + 0.25;
        if (pushDue(now, true, wake) && ended_) {
            // Everything was pushed. Freeze the clocks as a pause does.
            rebase(now);
            state_ = State::finished;
            doneCv_.notify_all();
            readerCv_.notify_all();
            continue;
        }
        if (wake > now) playerCv_.wait_for(lk, std::chrono::duration<double>(wake - now));
    }
}
