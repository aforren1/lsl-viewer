#pragma once
// Minimal XDF (Extensible Data Format) writer — the on-disk container LabRecorder
// produces, readable by pyxdf / sigviewer / EEGLAB. Adapted from
// labstreaminglayer/App-LabRecorder (xdfwriter), trimmed to what we need.
//
// Layout: the 4-byte magic "XDF:" followed by length-prefixed chunks
//   [varlen length][tag:u16][optional streamid:u32][content]
// Chunk tags: 1 FileHeader, 2 StreamHeader, 3 Samples, 4 ClockOffset, 5 Boundary,
// 6 StreamFooter. Multi-byte fields are little-endian (host order; asserted below).
//
// Thread-safe: one Writer is shared by all per-stream recording threads; each chunk
// is self-contained (carries its streamid), so chunks may be interleaved freely.

#include <bit>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xdf {

static_assert(std::endian::native == std::endian::little,
              "XDF writer assumes a little-endian host");

using streamid_t = std::uint32_t;

enum class tag : std::uint16_t {
    fileheader = 1, streamheader = 2, samples = 3,
    clockoffset = 4, boundary = 5, streamfooter = 6,
};

template <typename T>
inline void le(std::ostream& o, T v) {
    if constexpr (sizeof(T) == 1) o.put(static_cast<char>(v));
    else o.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

// Length prefix: [1|4|8 byte selector][value], smallest that fits.
inline void varlen(std::ostream& o, std::uint64_t v) {
    if (v < 256)              { o.put(1); o.put(static_cast<char>(static_cast<std::uint8_t>(v))); }
    else if (v <= 0xFFFFFFFF) { o.put(4); le(o, static_cast<std::uint32_t>(v)); }
    else                      { o.put(8); le(o, v); }
}
inline void fixlen4(std::ostream& o, std::uint32_t v) { o.put(4); le(o, v); }

// Per-sample timestamp: 0 = deduced (reader interpolates), else 8 + double.
inline void write_ts(std::ostream& o, double ts) {
    if (ts == 0.0) o.put(0); else { o.put(8); le(o, ts); }
}

template <class T>
inline const T* write_vals(std::ostream& o, const T* s, std::size_t n) {
    o.write(reinterpret_cast<const char*>(s), n * sizeof(T));
    return s + n;
}
inline const std::string* write_vals(std::ostream& o, const std::string* s, std::size_t n) {
    for (const std::string* end = s + n; s < end; ++s) {
        varlen(o, s->size());
        o.write(s->data(), static_cast<std::streamsize>(s->size()));
    }
    return s;
}

class Writer {
public:
    explicit Writer(const std::string& path) : f_(path, std::ios::binary) {
        if (!f_) throw std::runtime_error("cannot open file for writing: " + path);
        f_.write("XDF:", 4);
        chunk(tag::fileheader,
              "<?xml version=\"1.0\"?><info><version>1.0</version></info>", nullptr);
    }

    bool ok() const { return static_cast<bool>(f_); }

    void stream_header(streamid_t id, const std::string& xml) { lock l(m_); chunk(tag::streamheader, xml, &id); }
    void stream_footer(streamid_t id, const std::string& xml) { lock l(m_); chunk(tag::streamfooter, xml, &id); }

    void boundary() {
        static const std::uint8_t uuid[16] = {
            0x43, 0xA5, 0x46, 0xDC, 0xCB, 0xF5, 0x41, 0x0F,
            0xB3, 0x0E, 0xD5, 0x46, 0x73, 0x83, 0xCB, 0xE4};
        lock l(m_);
        chunk_header(tag::boundary, 16, nullptr);
        f_.write(reinterpret_cast<const char*>(uuid), 16);
    }

    void clock_offset(streamid_t id, double collection_time, double offset) {
        lock l(m_);
        chunk_header(tag::clockoffset, 2 * sizeof(double), &id);
        le(f_, collection_time); le(f_, offset);
    }

    // One Samples chunk: nsamp samples of nchan values of type T, with timestamps.
    template <class T>
    void data_chunk(streamid_t id, const std::vector<double>& ts,
                    const T* data, std::uint32_t nsamp, std::uint32_t nchan) {
        if (nsamp == 0) return;
        std::ostringstream out;
        fixlen4(out, nsamp);                         // sample count
        const T* p = data;
        for (std::uint32_t i = 0; i < nsamp; ++i) {
            write_ts(out, i < ts.size() ? ts[i] : 0.0);
            p = write_vals(out, p, nchan);
        }
        const std::string s = out.str();
        lock l(m_);
        chunk(tag::samples, s, &id);
    }

    std::uint64_t bytes() { lock l(m_); return static_cast<std::uint64_t>(f_.tellp()); }

private:
    using lock = std::lock_guard<std::mutex>;

    void chunk_header(tag t, std::size_t content_len, const streamid_t* id) {
        std::size_t len = content_len + sizeof(std::uint16_t);
        if (id) len += sizeof(streamid_t);
        varlen(f_, len);
        le(f_, static_cast<std::uint16_t>(t));
        if (id) le(f_, *id);
    }
    void chunk(tag t, const std::string& content, const streamid_t* id) {
        chunk_header(t, content.size(), id);
        f_.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::ofstream f_;
    std::mutex    m_;
};

}  // namespace xdf
