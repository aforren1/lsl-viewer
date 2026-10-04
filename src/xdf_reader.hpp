#pragma once
// Reads XDF files for replay: a header scan that builds an index of the Samples
// chunks, a chunk decoder, and the clock synchronization and dejitter of pyxdf.
// With the index, playback reads only the chunks it needs and can start anywhere,
// so memory use depends on the read-ahead and not on the size of the file.
//
// This is a port of tools/xdf_replay.py, which is the reference for every rule
// here. The timestamps it gives match pyxdf.load_xdf to well under 1 us (numpy
// rounds its least-squares solves differently, so the last bits can differ).
//
// Gzip-compressed files (.xdfz) are not supported. They would need zlib, and
// random access into a gzip stream means decompressing from the start.
//
// Layout (see xdf_writer.hpp): "XDF:" then chunks of
//   [varlen length][tag:u16][streamid:u32, tags 2/3/4/6 only][content]

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xdf {

static_assert(std::endian::native == std::endian::little,
              "XDF reader assumes a little-endian host");

using WarnFn = std::function<void(const std::string&)>;

// A chunk that cannot be used. scan() and the decoders recover from it as pyxdf
// does (skip to the next Boundary chunk, or skip the one chunk); the message goes
// into the warning.
struct Damaged : std::runtime_error { using std::runtime_error::runtime_error; };

// Seconds of slack around the window when chunks are picked by their first
// timestamp, because raw timestamps can step backwards across chunk borders.
inline constexpr double kMargin = 1.0;

enum class Format : std::uint8_t { float32, double64, int8, int16, int32, int64, string };

namespace detail {

struct EndOfFile {};

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
inline constexpr double kInf = std::numeric_limits<double>::infinity();

// Recorders write a Boundary chunk (tag 5) with this body every few seconds, so a
// reader that hits damage can find its place again.
inline constexpr std::uint8_t kBoundary[16] = {
    0x43, 0xA5, 0x46, 0xDC, 0xCB, 0xF5, 0x41, 0x0F,
    0xB3, 0x0E, 0xD5, 0x46, 0x73, 0x83, 0xCB, 0xE4};

inline bool parseFormat(std::string_view s, Format& f, std::size_t& bytes) {
    struct E { const char* name; Format f; std::size_t b; };
    static const E table[] = {
        {"float32", Format::float32, 4}, {"double64", Format::double64, 8},
        {"int8", Format::int8, 1},       {"int16", Format::int16, 2},
        {"int32", Format::int32, 4},     {"int64", Format::int64, 8},
        {"string", Format::string, 0}};
    for (const E& e : table)
        if (s == e.name) { f = e.f; bytes = e.b; return true; }
    return false;
}

inline std::string_view trim(std::string_view s) {
    const char* ws = " \t\r\n\f\v";
    const auto a = s.find_first_not_of(ws);
    if (a == std::string_view::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// Python's int() and float() accept surrounding whitespace; match that.
inline bool parseInt(std::string_view s, long long& v) {
    s = trim(s);
    if (!s.empty() && s[0] == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
inline bool parseDouble(std::string_view s, double& v) {
    // Not std::from_chars: libc++ has no floating-point version on older macOS. The
    // classic locale keeps the decimal point a period whatever the C locale is.
    s = trim(s);
    if (s.empty()) return false;
    std::istringstream in{std::string(s)};
    in.imbue(std::locale::classic());
    std::string word(s);
    for (char& c : word) c = (char)std::tolower((unsigned char)c);
    if (word == "inf" || word == "+inf" || word == "infinity" || word == "+infinity") { v = HUGE_VAL; return true; }
    if (word == "-inf" || word == "-infinity") { v = -HUGE_VAL; return true; }
    if (word == "nan" || word == "+nan" || word == "-nan") { v = std::numeric_limits<double>::quiet_NaN(); return true; }
    in >> v;
    return !in.fail() && in.peek() == std::char_traits<char>::eof();
}

// ---------------------------------------------------------------------------
// Minimal XML reader. The replay needs a few text fields of a stream header and
// the position of its <desc> element, which is copied to the outlet as recorded.
// ElementTree is the reference: an element's text is the character data before its
// first child, with entities decoded; comments and processing instructions are
// skipped. Malformed XML is rejected, because the scan treats it as damage.
// ---------------------------------------------------------------------------
struct XmlChild {
    std::string_view name;
    std::string      text;
    std::size_t      begin = 0, end = 0;   // the element in the source, tags included
};

inline void appendUtf8(std::string& o, std::uint32_t cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F));
    } else {
        o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F));
        o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F));
    }
}

// s[i] is '&'. Advances past the reference; appends its character to out if given.
inline bool xmlEntity(std::string_view s, std::size_t& i, std::string* out) {
    const std::size_t semi = s.find(';', i);
    if (semi == std::string_view::npos || semi - i > 12) return false;
    const std::string_view e = s.substr(i + 1, semi - i - 1);
    std::uint32_t cp = 0;
    if      (e == "lt")   cp = '<';
    else if (e == "gt")   cp = '>';
    else if (e == "amp")  cp = '&';
    else if (e == "quot") cp = '"';
    else if (e == "apos") cp = '\'';
    else if (e.size() > 1 && e[0] == '#') {
        const bool hex = e[1] == 'x';
        const std::string_view digits = e.substr(hex ? 2 : 1);
        if (digits.empty()) return false;
        const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
        if (r.ec != std::errc() || r.ptr != digits.data() + digits.size()) return false;
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    } else {
        return false;
    }
    if (out) appendUtf8(*out, cp);
    i = semi + 1;
    return true;
}

inline bool xmlNameStart(unsigned char c) {
    return c == '_' || c == ':' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80;
}
inline bool xmlNameChar(unsigned char c) {
    return xmlNameStart(c) || c == '-' || c == '.' || (c >= '0' && c <= '9');
}
inline bool xmlSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Parses one XML document (or one element) in s and lists the root's children.
// Returns false if it is not well formed.
inline bool parseXml(std::string_view s, std::vector<XmlChild>& kids) {
    kids.clear();
    std::size_t i = 0;
    const std::size_t n = s.size();
    if (s.substr(0, 3) == "\xEF\xBB\xBF") i = 3;
    auto skip = [&](std::string_view open, std::string_view close) -> int {   // 1 skipped, 0 no match, -1 bad
        if (s.compare(i, open.size(), open) != 0) return 0;
        const std::size_t e = s.find(close, i + open.size());
        if (e == std::string_view::npos) return -1;
        i = e + close.size();
        return 1;
    };
    auto misc = [&]() -> bool {   // whitespace, comments, PIs, and a DOCTYPE outside the root
        for (;;) {
            while (i < n && xmlSpace(s[i])) ++i;
            int r = skip("<!--", "-->");
            if (r == 0) r = skip("<?", "?>");
            if (r == 0 && s.compare(i, 9, "<!DOCTYPE") == 0) {
                int depth = 0;
                std::size_t j = i + 9;
                for (; j < n; ++j) {
                    if (s[j] == '[') ++depth;
                    else if (s[j] == ']') --depth;
                    else if (s[j] == '>' && depth <= 0) break;
                }
                if (j >= n) return false;
                i = j + 1;
                r = 1;
            }
            if (r < 0) return false;
            if (r == 0) return true;
        }
    };
    auto name = [&](std::string_view& out) -> bool {
        const std::size_t a = i;
        if (i >= n || !xmlNameStart((unsigned char)s[i])) return false;
        while (i < n && xmlNameChar((unsigned char)s[i])) ++i;
        out = s.substr(a, i - a);
        return true;
    };
    // Character data from i to the next '<'. Checks the entities and the characters.
    auto chars = [&](std::string* out) -> bool {
        while (i < n && s[i] != '<') {
            const unsigned char c = (unsigned char)s[i];
            if (c == '&') { if (!xmlEntity(s, i, out)) return false; continue; }
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
            if (out) *out += char(c);
            ++i;
        }
        return true;
    };

    if (!misc() || i >= n || s[i] != '<') return false;
    std::vector<std::string_view> stack;
    std::size_t cur = SIZE_MAX;      // index in kids of the open child of the root
    bool capture = false;            // still before that child's first own child
    for (;;) {
        if (i >= n) return false;    // the root is not closed
        if (s[i] != '<') {
            if (stack.empty()) return false;
            if (!chars(capture && stack.size() == 2 ? &kids[cur].text : nullptr)) return false;
            continue;
        }
        if (int r = skip("<!--", "-->"); r != 0) { if (r < 0) return false; continue; }
        if (s.compare(i, 9, "<![CDATA[") == 0) {
            const std::size_t e = s.find("]]>", i + 9);
            if (e == std::string_view::npos || stack.empty()) return false;
            if (capture && stack.size() == 2) kids[cur].text.append(s.substr(i + 9, e - i - 9));
            i = e + 3;
            continue;
        }
        if (int r = skip("<?", "?>"); r != 0) { if (r < 0) return false; continue; }
        if (s.compare(i, 2, "</") == 0) {
            i += 2;
            std::string_view nm;
            if (!name(nm)) return false;
            while (i < n && xmlSpace(s[i])) ++i;
            if (i >= n || s[i] != '>') return false;
            ++i;
            if (stack.empty() || stack.back() != nm) return false;
            stack.pop_back();
            if (stack.size() == 1 && cur != SIZE_MAX) { kids[cur].end = i; cur = SIZE_MAX; capture = false; }
            if (stack.empty()) break;
            continue;
        }
        // Start tag.
        const std::size_t tagStart = i++;
        std::string_view nm;
        if (!name(nm)) return false;
        bool selfClose = false;
        for (;;) {
            const std::size_t before = i;
            while (i < n && xmlSpace(s[i])) ++i;
            if (i >= n) return false;
            if (s[i] == '>') { ++i; break; }
            if (s.compare(i, 2, "/>") == 0) { i += 2; selfClose = true; break; }
            if (i == before) return false;            // attributes need white space before them
            std::string_view an;
            if (!name(an)) return false;
            while (i < n && xmlSpace(s[i])) ++i;
            if (i >= n || s[i] != '=') return false;
            ++i;
            while (i < n && xmlSpace(s[i])) ++i;
            if (i >= n || (s[i] != '"' && s[i] != '\'')) return false;
            const char q = s[i++];
            while (i < n && s[i] != q) {
                if (s[i] == '<') return false;
                if (s[i] == '&') { if (!xmlEntity(s, i, nullptr)) return false; continue; }
                ++i;
            }
            if (i >= n) return false;
            ++i;
        }
        if (stack.empty() && !kids.empty()) return false;
        if (stack.size() == 1) {
            kids.push_back({nm, {}, tagStart, selfClose ? i : 0});
            if (!selfClose) { cur = kids.size() - 1; capture = true; }
        } else if (stack.size() == 2) {
            capture = false;                         // text after a grandchild is its tail
        }
        if (selfClose) {
            if (stack.empty()) break;               // <root/>
            continue;
        }
        stack.push_back(nm);
    }
    return misc() && i == n;
}

inline const XmlChild* xmlFind(const std::vector<XmlChild>& kids, std::string_view name) {
    for (const XmlChild& k : kids) if (k.name == name) return &k;
    return nullptr;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// File access. One buffer serves the scan, which jumps from chunk header to chunk
// header; readAt() reads whole chunk bodies into the caller's buffer.
// ---------------------------------------------------------------------------
class InFile {
public:
    InFile() = default;
    InFile(const InFile&) = delete;
    InFile& operator=(const InFile&) = delete;
    ~InFile() { close(); }

    bool open(const std::filesystem::path& p) {
        close();
#ifdef _WIN32
        if (_wfopen_s(&f_, p.c_str(), L"rb") != 0) f_ = nullptr;
#else
        f_ = std::fopen(p.c_str(), "rb");
#endif
        if (!f_) return false;
        std::setvbuf(f_, nullptr, _IONBF, 0);     // buffered here instead, see fill()
        if (!rawSeek(0, SEEK_END)) { close(); return false; }
#ifdef _WIN32
        const long long end = _ftelli64(f_);
#else
        const long long end = ftello(f_);
#endif
        if (end < 0) { close(); return false; }
        size_ = (std::uint64_t)end;
        filePos_ = size_;
        base_ = 0; pos_ = len_ = 0; small_ = true;
        if (buf_.size() != kLarge) buf_.resize(kLarge);
        return true;
    }
    void close() { if (f_) std::fclose(f_); f_ = nullptr; }

    std::uint64_t size() const { return size_; }
    std::uint64_t tell() const { return base_ + pos_; }

    void seek(std::uint64_t off) {
        if (off >= base_ && off <= base_ + len_) { pos_ = (std::size_t)(off - base_); return; }
        base_ = off; pos_ = len_ = 0; small_ = true;
    }
    // Buffered read. Returns the bytes read, short at the end of the file.
    std::size_t read(void* dst, std::size_t n) {
        auto* d = static_cast<std::uint8_t*>(dst);
        std::size_t got = 0;
        while (got < n) {
            if (pos_ == len_ && !fill()) break;
            const std::size_t k = std::min(n - got, len_ - pos_);
            std::memcpy(d + got, buf_.data() + pos_, k);
            pos_ += k; got += k;
        }
        return got;
    }
    int get() {
        if (pos_ == len_ && !fill()) return -1;
        return buf_[pos_++];
    }
    // Reads n bytes at off into out, past the buffer. out keeps its capacity, so a
    // reused vector stops allocating once it has held the largest chunk. False on a
    // short read (a cut-off file).
    bool readAt(std::uint64_t off, std::size_t n, std::vector<std::uint8_t>& out) {
        out.resize(n);
        base_ = off; pos_ = len_ = 0; small_ = true;
        if (filePos_ != off && !rawSeek((long long)off, SEEK_SET)) { filePos_ = UINT64_MAX; return false; }
        const std::size_t got = n ? std::fread(out.data(), 1, n, f_) : 0;
        filePos_ = off + got;
        return got == n;
    }

private:
    // After a seek, read only a small block: the scan needs a few bytes of each chunk,
    // and chunks are often smaller than a full buffer, so full reads would read most
    // of the file. Reads that continue past the end of a block get the full size.
    static constexpr std::size_t kSmall = 4096, kLarge = 256 * 1024;

    bool fill() {
        const std::uint64_t at = base_ + pos_;
        const std::size_t want = small_ ? kSmall : kLarge;
        if (filePos_ != at && !rawSeek((long long)at, SEEK_SET)) { filePos_ = UINT64_MAX; return false; }
        const std::size_t got = std::fread(buf_.data(), 1, want, f_);
        filePos_ = at + got;
        base_ = at; pos_ = 0; len_ = got; small_ = false;
        return got > 0;
    }
    bool rawSeek(long long off, int whence) {
#ifdef _WIN32
        return _fseeki64(f_, off, whence) == 0;
#else
        return fseeko(f_, (off_t)off, whence) == 0;
#endif
    }

    std::FILE*                f_ = nullptr;
    std::vector<std::uint8_t> buf_;
    std::uint64_t             size_ = 0, base_ = 0, filePos_ = 0;
    std::size_t               pos_ = 0, len_ = 0;
    bool                      small_ = true;
};

namespace detail {

inline std::uint64_t readVarlen(InFile& f) {
    const int nb = f.get();
    if (nb < 0) throw EndOfFile{};
    if (nb == 1) {
        const int v = f.get();
        if (v < 0) throw Damaged("index out of range");
        return (std::uint64_t)v;
    }
    if (nb == 4) {
        std::uint32_t v;
        if (f.read(&v, 4) != 4) throw Damaged("unpack requires a buffer of 4 bytes");
        return v;
    }
    if (nb == 8) {
        std::uint64_t v;
        if (f.read(&v, 8) != 8) throw Damaged("unpack requires a buffer of 8 bytes");
        return v;
    }
    throw Damaged("invalid length-byte count " + std::to_string(nb) + " at offset " +
                  std::to_string(f.tell() - 1));
}

inline std::uint64_t readVarlen(const std::uint8_t* b, std::size_t size, std::size_t& q) {
    if (q >= size) throw Damaged("index out of range");
    const std::uint8_t nb = b[q];
    if (nb == 1) {
        if (q + 2 > size) throw Damaged("index out of range");
        q += 2;
        return b[q - 1];
    }
    if (nb == 4 || nb == 8) {
        if (q + 1 + nb > size) throw Damaged("unpack requires a buffer of " + std::to_string(nb) + " bytes");
        std::uint64_t v = 0;
        std::memcpy(&v, b + q + 1, nb);
        q += 1 + nb;
        return v;
    }
    throw Damaged("invalid length-byte count " + std::to_string(nb));
}

// Seeks to just after the next Boundary signature at or after pos. Returns false at
// the end of the file.
inline bool scanForward(InFile& f, std::uint64_t pos, std::uint64_t& resume) {
    constexpr std::size_t kSig = sizeof(kBoundary);
    std::vector<std::uint8_t> buf(kSig - 1 + (1u << 20));
    std::size_t tail = 0;
    f.seek(pos);
    for (;;) {
        const std::size_t got = f.read(buf.data() + tail, buf.size() - tail);
        if (!got) return false;
        const std::size_t len = tail + got;
        // The tail of the previous block is kept, so a signature across the seam is found.
        const auto hit = std::search(buf.begin(), buf.begin() + (std::ptrdiff_t)len,
                                     std::begin(kBoundary), std::end(kBoundary));
        if (hit != buf.begin() + (std::ptrdiff_t)len) {
            resume = f.tell() - len + (std::uint64_t)(hit - buf.begin()) + kSig;
            f.seek(resume);
            return true;
        }
        tail = std::min(len, kSig - 1);
        std::memmove(buf.data(), buf.data() + len - tail, tail);
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Header scan and chunk index. Reads only chunk headers, stream headers, footers,
// clock offsets, and the first bytes of each Samples chunk, so it is fast even on
// multi-GB files.
// ---------------------------------------------------------------------------
struct ChunkRef {
    std::uint64_t off;        // file offset after the stream id
    std::uint64_t end;        // end of the chunk
    std::uint64_t n;          // samples in the chunk
    std::uint64_t g;          // index of the chunk's first sample in the stream
    double        ref;        // raw timestamp of the first sample, NaN if unknown
    bool          firstExp;   // the first sample has its own timestamp
    bool          hasExp;     // some sample has its own timestamp
};

struct StreamHeader {
    std::uint32_t id = 0;
    std::string   name, type, sourceId, formatName;
    std::string   desc;                 // the <desc> element as recorded, empty if none
    Format        format = Format::float32;
    int           channels = 0;
    double        srate = 0.0;          // 0 for irregular streams
    std::size_t   valueSize = 0;        // bytes per numeric sample; 0 for strings
    double        tdiff = 0.0;          // 1 / srate, 0 for irregular streams
    bool          canDropSamples = false;
    std::uint64_t count = 0;            // samples in the indexed chunks
    std::optional<double>        first; // timestamp of the first sample (pyxdf's deduction)
    std::optional<double>        last;  // raw timestamp of the last sample
    std::optional<std::uint64_t> footerCount;
    std::vector<double>   clockTimes, clockValues;
    std::vector<ChunkRef> chunks;
};

namespace detail {

// Decodes one Samples chunk just far enough to get its last timestamp.
inline std::optional<double> lastTimestamp(InFile& f, const StreamHeader& s) {
    try {
        f.seek(s.chunks.back().off);
        const std::uint64_t count = readVarlen(f);
        std::optional<double> ts;
        for (std::uint64_t i = 0; i < count; ++i) {
            const int t = f.get();
            if (t < 0) return std::nullopt;
            if (t) {
                double v;
                if (f.read(&v, 8) != 8) return std::nullopt;
                ts = v;
            } else if (ts && s.srate > 0) {
                *ts += 1.0 / s.srate;
            }
            if (s.valueSize) f.seek(f.tell() + s.valueSize);
            else for (int c = 0; c < s.channels; ++c) {
                const std::uint64_t ln = readVarlen(f);
                f.seek(f.tell() + ln);
            }
        }
        return ts;
    } catch (...) {
        return std::nullopt;
    }
}

inline void parseHeader(StreamHeader& s, std::string_view xml) {
    std::vector<XmlChild> kids;
    if (!parseXml(xml, kids)) throw Damaged("stream header is not valid XML");
    auto text = [&](std::string_view k) -> std::optional<std::string> {
        const XmlChild* c = xmlFind(kids, k);
        return c ? std::optional<std::string>(c->text) : std::nullopt;
    };
    const auto fmt = text("channel_format");
    if (!fmt || !parseFormat(*fmt, s.format, s.valueSize))
        throw Damaged("unsupported channel_format '" + fmt.value_or("") + "'");
    s.formatName = *fmt;
    long long nch = 0;
    if (!parseInt(text("channel_count").value_or(""), nch) || nch < 0 || nch > (1 << 24))
        throw Damaged("invalid channel_count");
    s.channels = (int)nch;
    const std::string sr = text("nominal_srate").value_or("");
    s.srate = 0.0;
    if (!sr.empty() && !parseDouble(sr, s.srate)) throw Damaged("invalid nominal_srate");
    s.name     = text("name").value_or("");
    s.type     = text("type").value_or("");
    s.sourceId = text("source_id").value_or("");
    s.valueSize *= (std::size_t)s.channels;
    s.tdiff = s.srate > 0 ? 1.0 / s.srate : 0.0;
    s.desc.clear();
    s.canDropSamples = false;
    if (const XmlChild* d = xmlFind(kids, "desc")) {
        s.desc.assign(xml.substr(d->begin, d->end - d->begin));
        // pyxdf skips dejitter when desc/synchronization/can_drop_samples is "true".
        std::vector<XmlChild> dk, sk;
        if (parseXml(s.desc, dk))
            if (const XmlChild* sy = xmlFind(dk, "synchronization"))
                if (parseXml(std::string_view(s.desc).substr(sy->begin, sy->end - sy->begin), sk))
                    if (const XmlChild* c = xmlFind(sk, "can_drop_samples")) {
                        std::string t = c->text;
                        for (char& ch : t) ch = (char)std::tolower((unsigned char)ch);
                        s.canDropSamples = t == "true";
                    }
    }
}

}  // namespace detail

// Returns the streams in the order of their headers, each with its chunk index.
// Throws std::runtime_error if the file cannot be read or is not XDF; damage
// inside the file only produces warnings. cancel, if set, aborts the scan.
inline std::vector<StreamHeader> scan(const std::filesystem::path& path, const WarnFn& warn = {},
                                      const std::atomic<bool>* cancel = nullptr) {
    using namespace detail;
    InFile f;
    if (!f.open(path)) throw std::runtime_error(path.string() + ": cannot open the file");
    std::uint8_t magic[4] = {};
    const std::size_t got = f.read(magic, 4);
    if (got >= 2 && magic[0] == 0x1f && magic[1] == 0x8b)
        throw std::runtime_error(path.string() + ": gzip-compressed XDF is not supported, decompress it first");
    if (got != 4 || std::memcmp(magic, "XDF:", 4) != 0)
        throw std::runtime_error(path.string() + ": not an XDF file");

    std::vector<StreamHeader> streams;
    std::unordered_map<std::uint32_t, std::size_t> slot;
    std::vector<char> text;
    const std::uint64_t size = f.size();
    std::uint64_t nchunks = 0;
    for (;;) {
        if (cancel && (++nchunks & 4095) == 0 && cancel->load(std::memory_order_relaxed))
            throw std::runtime_error("cancelled");
        const std::uint64_t start = f.tell();
        std::uint64_t body = 0, length = 0;
        try {
            length = readVarlen(f);
            body = f.tell();
            if (length > size - body) throw Damaged("chunk runs past the end of the file");
            std::uint16_t tag;
            if (f.read(&tag, 2) != 2) throw Damaged("unpack requires a buffer of 2 bytes");
            std::uint32_t sid = 0;
            if (tag == 2 || tag == 3 || tag == 4 || tag == 6) {
                if (length < 6) throw Damaged("chunk too short for its stream id");
                if (f.read(&sid, 4) != 4) throw Damaged("unpack requires a buffer of 4 bytes");
            }
            const auto it = slot.find(sid);
            if (tag == 2) {
                text.resize((std::size_t)(length - 6));
                if (f.read(text.data(), text.size()) != text.size()) throw Damaged("cut-off stream header");
                StreamHeader h;
                h.id = sid;
                detail::parseHeader(h, std::string_view(text.data(), text.size()));
                // A repeated header replaces the stream but keeps its place, as in a dict.
                if (it == slot.end()) { slot[sid] = streams.size(); streams.push_back(std::move(h)); }
                else streams[it->second] = std::move(h);
            } else if (tag == 3 && it != slot.end()) {
                StreamHeader& s = streams[it->second];
                const std::uint64_t off = f.tell();
                const std::uint64_t n = readVarlen(f);
                const std::int64_t rest = (std::int64_t)(body + length) - (std::int64_t)f.tell();
                double ref = kNaN;
                bool firstExp = false, hasExp = false;
                if (n) {
                    int t = f.get();
                    if (t < 0) throw Damaged("index out of range");
                    firstExp = t != 0;
                    if (s.valueSize) {
                        // Each sample is 1 + valueSize bytes plus 8 if it has a timestamp, so
                        // the length tells how many have none (k).
                        if (rest < 0 || n > (std::uint64_t)rest / (1 + s.valueSize))
                            throw Damaged("sample sizes do not match the chunk length");
                        const std::int64_t d = (std::int64_t)(n * (9 + s.valueSize)) - rest;
                        if (d < 0 || d % 8 || (std::uint64_t)(d / 8) > n)
                            throw Damaged("sample sizes do not match the chunk length");
                        hasExp = (std::uint64_t)(d / 8) < n;
                        // Leading samples without a timestamp have a fixed size, so the first
                        // stamped one is a few seeks away.
                        std::uint64_t j = 0;
                        const std::uint64_t jmax = std::min<std::uint64_t>(n - 1, 64);
                        while (!t && j < jmax) {
                            f.seek(f.tell() + s.valueSize);
                            t = f.get();
                            if (t < 0) throw Damaged("index out of range");
                            ++j;
                        }
                        if (t) {
                            double v;
                            if (f.read(&v, 8) != 8) throw Damaged("unpack requires a buffer of 8 bytes");
                            ref = v - (double)j * s.tdiff;
                        }
                    } else {
                        // A string sample takes at least its tag byte.
                        if (rest < 0 || n > (std::uint64_t)rest) throw Damaged("sample count exceeds the chunk length");
                        hasExp = firstExp;
                        if (t) {
                            double v;
                            if (f.read(&v, 8) != 8) throw Damaged("unpack requires a buffer of 8 bytes");
                            ref = v;
                        }
                    }
                    // pyxdf starts each stream's deduced timestamps from 0.0.
                    if (!s.first) s.first = firstExp ? ref : s.tdiff;
                }
                s.count += n;
                s.chunks.push_back({off, body + length, n, 0, ref, firstExp, hasExp});
            } else if (tag == 4 && it != slot.end()) {
                double tv[2];
                if (f.read(tv, 16) != 16) throw Damaged("unpack requires a buffer of 16 bytes");
                streams[it->second].clockTimes.push_back(tv[0]);
                streams[it->second].clockValues.push_back(tv[1]);
            } else if (tag == 6 && it != slot.end()) {
                text.resize((std::size_t)(length - 6));
                if (f.read(text.data(), text.size()) == text.size()) {
                    std::vector<XmlChild> kids;
                    if (parseXml(std::string_view(text.data(), text.size()), kids)) {
                        const XmlChild* c = xmlFind(kids, "sample_count");
                        long long v = 0;
                        if (!c || c->text.empty()) streams[it->second].footerCount.reset();
                        else if (parseInt(c->text, v)) streams[it->second].footerCount = (std::uint64_t)v;
                    }
                }
            }
        } catch (const EndOfFile&) {
            break;
        } catch (const Damaged&) {
            // A recorder that crashed leaves a cut-off last chunk. Damage in the middle
            // loses only the data up to the next Boundary chunk.
            std::uint64_t resume = 0;
            if (!scanForward(f, start + 1, resume)) {
                if (warn) warn("damaged or cut-off chunk near byte " + std::to_string(start) +
                               ", file read up to there");
                break;
            }
            if (warn) warn("damaged chunk near byte " + std::to_string(start) +
                           ", skipped to the boundary at byte " + std::to_string(resume));
            continue;
        }
        f.seek(body + length);
    }
    for (StreamHeader& s : streams) {
        std::uint64_t g = 0;
        for (ChunkRef& c : s.chunks) { c.g = g; g += c.n; }
        if (!s.chunks.empty()) s.last = detail::lastTimestamp(f, s);
    }
    return streams;
}

// ---------------------------------------------------------------------------
// Clock sync and dejitter. These port the algorithms of pyxdf 1.17
// (_truncate_corrupted_offsets, _clock_sync, _robust_fit, _jitter_removal) with
// its default parameters, so that the replayed timestamps are the ones that
// pyxdf.load_xdf returns. pyxdf notice:
//
// BSD 2-Clause License
// Copyright (c) 2015-2024, Syntrogi Inc. dba Intheon
// Copyright (c) 2018-2024, Chad Boulay
// Copyright (c) 2018-2024, Tristan Stenner
// Copyright (c) 2018-2024, Clemens Brunner
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// ---------------------------------------------------------------------------
namespace detail {

inline constexpr double kWinsor = 0.0001;
inline constexpr double kEps = 2.220446049250313e-16;   // np.finfo(float).eps

// np.median: the middle value, or the mean of the two middle values.
inline double median(std::vector<double> v) {
    if (v.empty()) return kNaN;
    const std::size_t h = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)h, v.end());
    const double hi = v[h];
    if (v.size() % 2) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + (std::ptrdiff_t)h);
    return (lo + hi) / 2.0;
}

// (start, end) index pairs of the parts between breaks, both ends inclusive.
inline std::vector<std::pair<std::size_t, std::size_t>> segments(const std::vector<bool>& breaks) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t a = 0;
    for (std::size_t i = 0; i < breaks.size(); ++i)
        if (breaks[i]) { out.push_back({a, i}); a = i + 1; }
    out.push_back({a, breaks.size()});
    return out;
}

inline std::vector<bool> clockGlitch(const std::vector<double>& diff, double threshStds, double threshSecs) {
    const double med = median(diff);
    std::vector<double> shiftAbs(diff.size());
    for (std::size_t i = 0; i < diff.size(); ++i) shiftAbs[i] = std::abs(diff[i] - med);
    const double mad = median(shiftAbs) + kEps;
    std::vector<bool> out(diff.size());
    for (std::size_t i = 0; i < diff.size(); ++i)
        out[i] = std::abs((diff[i] - med) / mad) > threshStds && shiftAbs[i] > threshSecs;
    return out;
}

// pylsl#67: an outlet that closes can send one extra sample with a corrupted clock
// offset after it. pyxdf drops both when the last offset is an outlier.
inline bool lastOffsetCorrupted(const std::vector<double>& times, const std::vector<double>& values) {
    if (times.size() < 3) return false;
    std::vector<double> iv(times.size() - 1);
    for (std::size_t i = 0; i + 1 < times.size(); ++i) iv[i] = times[i + 1] - times[i];
    const double medianInterval = median(std::vector<double>(iv.begin(), iv.end() - 1));
    const double lastInterval = std::abs(iv.back());
    double timeRatio;
    if (medianInterval > 0) timeRatio = lastInterval / medianInterval;
    else timeRatio = lastInterval > 0 ? kInf : 1.0;
    std::vector<double> head(values.begin(), values.end() - 1);
    const double medianVal = median(head);
    for (double& v : head) v = std::abs(v - medianVal);
    const double mad = median(head);
    const double z = mad > kEps ? std::abs(values.back() - medianVal) / (1.4826 * mad) : 0.0;
    return timeRatio > 10.0 || z > 10.0;
}

// Huber-loss line fit by ADMM, as in pyxdf, on A = [1, t / W], y = v / W.
// Returns false where numpy's Cholesky would raise LinAlgError.
inline bool robustFit(const double* t, const double* v, std::size_t n, double& c0, double& c1) {
    std::vector<double> x(n), y(n), z(n, 0.0), u(n, 0.0);
    double offset = kInf;
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = t[i] / kWinsor;
        y[i] = v[i] / kWinsor;
        offset = std::min(offset, x[i]);
    }
    double a12 = 0, a22 = 0, aty0 = 0, aty1 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        x[i] -= offset;
        a12 += x[i]; a22 += x[i] * x[i];
        aty0 += y[i]; aty1 += x[i] * y[i];
    }
    const double a11 = (double)n;
    if (!(a11 > 0)) return false;
    const double l11 = std::sqrt(a11), l21 = a12 / l11, r = a22 - l21 * l21;
    if (!(r > 0)) return false;
    const double l22 = std::sqrt(r);
    double x0 = 0, x1 = 0;
    for (int it = 0; it < 1000; ++it) {
        double s0 = 0, s1 = 0;
        for (std::size_t i = 0; i < n; ++i) { const double w = z[i] - u[i]; s0 += w; s1 += x[i] * w; }
        const double y0 = (aty0 + s0) / l11;
        const double y1 = ((aty1 + s1) - l21 * y0) / l22;
        x1 = y1 / l22;
        x0 = (y0 - l21 * x1) / l11;
        for (std::size_t i = 0; i < n; ++i) {
            const double d = x0 + x[i] * x1 - y[i] + u[i];
            const double dinv = d != 0 ? 1.0 / d : 0.0;
            const double tmp = std::max(0.0, 1.0 - 2.0 * std::abs(dinv));   // rho = 1
            z[i] = 0.5 * d + 0.5 * tmp * d;
            u[i] = d - z[i];
        }
    }
    x0 -= x1 * offset;
    c0 = x0 * kWinsor;
    c1 = x1;
    return true;
}

}  // namespace detail

// Maps a stream's raw timestamps to the ones pyxdf.load_xdf returns.
class Timebase {
public:
    Timebase() = default;
    Timebase(const StreamHeader& s, bool sync, const WarnFn& warn = {}) {
        sync_ = sync && s.count > 0 && !s.clockTimes.empty();
        times_ = s.clockTimes;
        std::vector<double> values = s.clockValues;
        if (sync && s.footerCount && s.count > *s.footerCount &&
            detail::lastOffsetCorrupted(times_, values)) {
            times_.pop_back();
            values.pop_back();
            limit = *s.footerCount;
        }
        if (!sync_ || times_.empty()) { sync_ = false; return; }
        if (times_.size() > 1) {
            std::vector<double> dt(times_.size() - 1), dv(times_.size() - 1);
            for (std::size_t i = 0; i + 1 < times_.size(); ++i) {
                dt[i] = times_[i + 1] - times_[i];
                dv[i] = values[i + 1] - values[i];
            }
            const auto gt = detail::clockGlitch(dt, 5, 5), gv = detail::clockGlitch(dv, 10, 1);
            std::vector<bool> resets(dt.size());
            for (std::size_t i = 0; i < dt.size(); ++i) resets[i] = dt[i] < 0 || (gt[i] && gv[i]);
            ranges_ = detail::segments(resets);
        } else {
            ranges_ = {{0, 0}};
        }
        for (const auto& [a, b] : ranges_) {
            if (a != b) {
                double c0 = 0, c1 = 0;
                if (!detail::robustFit(times_.data() + a, values.data() + a, b + 1 - a, c0, c1)) {
                    if (warn) warn("stream " + std::to_string(s.id) + ": clock offsets (" + std::to_string(a) +
                                   ", " + std::to_string(b) + ") cannot be used for sync");
                    c0 = c1 = 0;
                }
                coefs_.push_back({c0, c1});
            } else {
                coefs_.push_back({values[a], 0.0});
            }
        }
    }

    // Sample count after pyxdf drops a pylsl#67 extra sample.
    std::optional<std::uint64_t> limit;

    bool synced() const { return sync_; }
    // Several clock ranges: which range a sample belongs to depends on all of the
    // stream's timestamps, so they must be read first (assignRanges).
    bool needsPrepass() const { return sync_ && ranges_.size() > 1; }
    bool dejittered() const { return !dejitStart_.empty(); }

    // pyxdf's split of all samples into clock ranges; raw holds every raw timestamp.
    void assignRanges(const std::vector<double>& raw) {
        bounds_.clear();
        std::size_t tsStart = 0;
        for (const auto& [a, b] : ranges_) {
            const std::size_t stop = b + 1;
            std::size_t tsStop;
            if (stop < times_.size()) {
                // The first sample that is closer to the next range's first offset.
                tsStop = raw.size();
                for (std::size_t i = tsStart; i < raw.size(); ++i)
                    if (!(std::abs(raw[i] - times_[b]) < std::abs(raw[i] - times_[stop]))) { tsStop = i; break; }
            } else {
                tsStop = raw.size();
            }
            bounds_.push_back(tsStart != tsStop ? (std::int64_t)tsStart : -1);
            tsStart = tsStop;
        }
        liveStart_.clear(); liveRange_.clear();
        for (std::size_t r = 0; r < bounds_.size(); ++r)
            if (bounds_[r] >= 0) { liveStart_.push_back((std::uint64_t)bounds_[r]); liveRange_.push_back(r); }
    }

    // Clock-corrected timestamp of sample idx with raw timestamp raw.
    double clock(double raw, std::uint64_t idx) const {
        if (!sync_) return raw;
        const auto& c = coefs_[rangeFor(raw, idx)];
        return raw + (c.first + c.second * raw);
    }
    // In place, for samples g0, g0 + 1, ...
    void clock(double* t, std::uint64_t g0, std::size_t n) const {
        if (!sync_) return;
        if (ranges_.size() == 1) {
            const auto c = coefs_[0];
            for (std::size_t i = 0; i < n; ++i) t[i] = t[i] + (c.first + c.second * t[i]);
            return;
        }
        for (std::size_t i = 0; i < n; ++i) t[i] = clock(t[i], g0 + i);
    }

    // pyxdf's jitter removal: split at breaks, then a least-squares line per part.
    // ts holds every clock-corrected timestamp of the stream.
    void fitDejitter(const std::vector<double>& ts, const StreamHeader& s) {
        dejitStart_.clear(); dejitM0_.clear(); dejitM1_.clear();
        if (s.srate == 0 || ts.empty() || s.canDropSamples) return;
        const double thresh = std::max(1.0, 500 * s.tdiff);
        std::vector<bool> breaks(ts.size() - 1);
        for (std::size_t i = 0; i + 1 < ts.size(); ++i) breaks[i] = std::abs(ts[i + 1] - ts[i]) > thresh;
        for (const auto& [a, b] : detail::segments(breaks)) {
            double m0, m1;
            lineFit(ts, a, b, m0, m1);
            dejitStart_.push_back(a);
            dejitM0_.push_back(m0);
            dejitM1_.push_back(m1);
        }
    }

    // Timestamps as pyxdf returns them.
    double final(double raw, std::uint64_t idx) const {
        if (dejitStart_.empty()) return clock(raw, idx);
        const std::size_t seg = segmentOf(idx);
        return dejitM0_[seg] + dejitM1_[seg] * (double)idx;
    }
    void final(double* t, std::uint64_t g0, std::size_t n) const {
        if (dejitStart_.empty()) { clock(t, g0, n); return; }
        std::size_t seg = segmentOf(g0);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t idx = g0 + i;
            while (seg + 1 < dejitStart_.size() && dejitStart_[seg + 1] <= idx) ++seg;
            t[i] = dejitM0_[seg] + dejitM1_[seg] * (double)idx;
        }
    }

private:
    // Least squares of ts[a..b] on the sample index. numpy solves it by SVD; this
    // centers the index and accumulates with compensation, which is as accurate.
    static void lineFit(const std::vector<double>& ts, std::size_t a, std::size_t b, double& m0, double& m1) {
        if (a == b) {
            // One point: numpy returns the minimum-norm solution.
            const double ia = (double)a, den = 1.0 + ia * ia;
            m0 = ts[a] / den;
            m1 = ia * ts[a] / den;
            return;
        }
        const double n = (double)(b - a + 1);
        const double xm = 0.5 * ((double)a + (double)b);
        const double t0 = ts[a];
        double sy = 0, cy = 0, sxy = 0, cxy = 0;   // Neumaier sums
        auto add = [](double& s, double& c, double v) {
            const double t = s + v;
            c += std::abs(s) >= std::abs(v) ? (s - t) + v : (v - t) + s;
            s = t;
        };
        for (std::size_t i = a; i <= b; ++i) {
            const double dy = ts[i] - t0;
            add(sy, cy, dy);
            add(sxy, cxy, ((double)i - xm) * dy);
        }
        const double sxx = n * (n * n - 1.0) / 12.0;   // sum of (i - xm)^2
        m1 = (sxy + cxy) / sxx;
        const double ymean = t0 + (sy + cy) / n;
        m0 = ymean - m1 * xm;
    }

    std::size_t rangeFor(double raw, std::uint64_t idx) const {
        if (ranges_.size() == 1) return 0;
        if (bounds_.empty()) {
            // Before assignRanges (single values such as the first sample): the range
            // whose end is closer, as pyxdf decides it.
            std::size_t r = 0;
            while (r + 1 < ranges_.size()) {
                const std::size_t stop = ranges_[r].second + 1;
                if (std::abs(raw - times_[ranges_[r].second]) < std::abs(raw - times_[stop])) return r;
                ++r;
            }
            return r;
        }
        const auto it = std::upper_bound(liveStart_.begin(), liveStart_.end(), idx);
        if (it == liveStart_.begin()) return liveRange_.empty() ? 0 : liveRange_.back();   // numpy's [-1]
        return liveRange_[(std::size_t)(it - liveStart_.begin()) - 1];
    }
    std::size_t segmentOf(std::uint64_t idx) const {
        const auto it = std::upper_bound(dejitStart_.begin(), dejitStart_.end(), idx);
        return it == dejitStart_.begin() ? dejitStart_.size() - 1 : (std::size_t)(it - dejitStart_.begin()) - 1;
    }

    bool sync_ = false;
    std::vector<double> times_;
    std::vector<std::pair<std::size_t, std::size_t>> ranges_;
    std::vector<std::pair<double, double>> coefs_;
    std::vector<std::int64_t>  bounds_;        // first sample of each range, -1 if none
    std::vector<std::uint64_t> liveStart_;     // the ranges that have samples
    std::vector<std::size_t>   liveRange_;
    std::vector<std::uint64_t> dejitStart_;
    std::vector<double>        dejitM0_, dejitM1_;
};

// ---------------------------------------------------------------------------
// Samples chunk decoding
// ---------------------------------------------------------------------------

// The decoded samples of one chunk. The caller keeps and reuses it, so decoding
// does not allocate once the buffers have grown to the largest chunk.
struct Samples {
    std::size_t                n = 0;
    std::vector<double>        ts;
    std::vector<std::uint8_t>  values;   // numeric: n * valueSize bytes
    std::vector<const char*>   str;      // strings: n * channels pointers into the chunk bytes
    std::vector<std::uint32_t> len;      // and their lengths
};

// Decodes one Samples chunk: buf holds its bytes after the stream id. last is the
// stream's previous timestamp. A sample without its own timestamp gets the previous
// one plus 1/srate, added in sequence as pyxdf does, so long runs accumulate the
// same rounding. last is updated only on success. Throws Damaged.
inline void decode(const StreamHeader& s, const std::uint8_t* buf, std::size_t size, double& last,
                   bool wantValues, Samples& out) {
    std::size_t q = 0;
    const std::uint64_t n64 = detail::readVarlen(buf, size, q);
    const std::size_t vs = s.valueSize;
    if (!vs) {
        // Strings: each value is length-prefixed, so the samples are walked one by one.
        // Each sample takes at least its tag byte and a 2-byte length per value. A
        // damaged count must fail here, before it sizes the pointer arrays.
        const std::size_t nch = (std::size_t)s.channels;
        if (n64 > size / (1 + 2 * nch)) throw Damaged("sample count exceeds the chunk length");
        const std::size_t n = (std::size_t)n64;
        out.ts.resize(n);
        if (wantValues) { out.str.resize(n * nch); out.len.resize(n * nch); }
        double prev = last;
        for (std::size_t i = 0; i < n; ++i) {
            if (q >= size) throw Damaged("index out of range");
            if (buf[q]) {
                if (q + 9 > size) throw Damaged("unpack requires a buffer of 8 bytes");
                std::memcpy(&prev, buf + q + 1, 8);
                q += 9;
            } else {
                prev += s.tdiff;
                q += 1;
            }
            out.ts[i] = prev;
            for (std::size_t c = 0; c < nch; ++c) {
                const std::uint64_t ln = detail::readVarlen(buf, size, q);
                // A value that runs past the end is cut, as a Python slice would; the
                // next read then fails.
                const std::size_t have = std::min<std::uint64_t>(ln, size - q);
                if (wantValues) {
                    out.str[i * nch + c] = reinterpret_cast<const char*>(buf + q);
                    out.len[i * nch + c] = (std::uint32_t)have;
                }
                q = ln > size - q ? size + 1 : q + (std::size_t)ln;
            }
        }
        out.n = n;
        if (n) last = prev;
        return;
    }
    const std::size_t rest = size - q;
    if (n64 > rest / (1 + vs)) throw Damaged("sample sizes do not match the chunk length");
    const std::size_t n = (std::size_t)n64;
    out.n = n;
    out.ts.resize(n);
    if (wantValues) out.values.resize(n * vs);
    if (!n) return;
    const std::int64_t d = (std::int64_t)(n * (9 + vs)) - (std::int64_t)rest;
    if (d < 0 || d % 8 || (std::size_t)(d / 8) > n) throw Damaged("sample sizes do not match the chunk length");
    const std::size_t k = (std::size_t)(d / 8);        // samples without a timestamp
    const std::uint8_t* p = buf + q;
    std::uint8_t* val = out.values.data();
    if (k == 0) {
        for (std::size_t i = 0; i < n; ++i)
            if (!p[i * (9 + vs)]) throw Damaged("unstamped sample in a chunk sized for stamps only");
        for (std::size_t i = 0; i < n; ++i, p += 9 + vs) {
            std::memcpy(&out.ts[i], p + 1, 8);
            if (wantValues) std::memcpy(val + i * vs, p + 9, vs);
        }
    } else if (k == n) {
        for (std::size_t i = 0; i < n; ++i)
            if (p[i * (1 + vs)]) throw Damaged("timestamp flags do not match the chunk length");
        double prev = last;
        for (std::size_t i = 0; i < n; ++i, p += 1 + vs) {
            prev += s.tdiff;
            out.ts[i] = prev;
            if (wantValues) std::memcpy(val + i * vs, p + 1, vs);
        }
    } else {
        // Each tag byte decides where the next one is, so the tags are walked first;
        // recorders store a stamp only where it differs from the deduced one, so there
        // is no pattern to predict.
        std::size_t at = q, stamped = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (at >= size) throw Damaged("index out of range");
            if (buf[at]) { at += 9 + vs; ++stamped; }
            else at += 1 + vs;
        }
        if (n - stamped != k) throw Damaged("timestamp flags do not match the chunk length");
        double prev = last;
        for (std::size_t i = 0; i < n; ++i) {
            if (*p) { std::memcpy(&prev, p + 1, 8); p += 9; }
            else { prev += s.tdiff; p += 1; }
            out.ts[i] = prev;
            if (wantValues) std::memcpy(val + i * vs, p, vs);
            p += vs;
        }
    }
    last = out.ts[n - 1];
}

// ---------------------------------------------------------------------------
// Chunk start times and plausibility
// ---------------------------------------------------------------------------

// Start time of every chunk, usable to seek and to pace the reader. raw holds the
// clock-corrected first stamps (raw clocks can reset backwards). A chunk can have
// no stamp near its start, and a damaged chunk with an intact length yields a
// nonsense one. Either would make a seek pick the wrong chunk or stall the reader,
// so only starts on the longest non-decreasing run are kept (gaps in the recording
// only move forward), and the others are extrapolated from the last kept one.
inline std::vector<double> chunkStarts(std::vector<double> raw, const std::vector<ChunkRef>& ch, double tdiff) {
    const std::size_t n = raw.size();
    std::vector<char> keep(n, 0);
    std::vector<std::size_t> fin;
    for (std::size_t i = 0; i < n; ++i) if (std::isfinite(raw[i])) fin.push_back(i);
    if (!fin.empty()) {
        std::vector<double> tails;
        std::vector<std::size_t> tailAt;
        std::vector<std::ptrdiff_t> parent(fin.size(), -1);
        for (std::size_t i = 0; i < fin.size(); ++i) {
            const double v = raw[fin[i]];
            const std::size_t k = (std::size_t)(std::upper_bound(tails.begin(), tails.end(), v) - tails.begin());
            if (k == tails.size()) { tails.push_back(v); tailAt.push_back(i); }
            else { tails[k] = v; tailAt[k] = i; }
            parent[i] = k ? (std::ptrdiff_t)tailAt[k - 1] : -1;
        }
        for (std::ptrdiff_t i = (std::ptrdiff_t)tailAt.back(); i >= 0; i = parent[(std::size_t)i])
            keep[fin[(std::size_t)i]] = 1;
    }
    if (std::all_of(keep.begin(), keep.end(), [](char k) { return k != 0; })) return raw;
    std::vector<double> out(n);
    std::ptrdiff_t prev = -1;
    for (std::size_t i = 0; i < n; ++i) {
        if (keep[i]) prev = (std::ptrdiff_t)i;
        // pyxdf deduces a stream's leading unstamped samples from 0.0.
        out[i] = prev >= 0
            ? raw[(std::size_t)prev] + (double)(std::int64_t)(ch[i].g - ch[(std::size_t)prev].g) * tdiff
            : (double)(ch[i].g + 1) * tdiff;
    }
    return out;
}

// False when a decoded chunk's stamps lie outside its neighbors' starts:
// misaligned bytes after damage can still pass the chunk's size checks.
inline bool plausible(const double* ts, std::size_t n, const std::vector<double>& ref, std::size_t c) {
    double lo = detail::kInf, hi = -detail::kInf;
    for (std::size_t i = 0; i < n; ++i)
        if (std::isfinite(ts[i])) { lo = std::min(lo, ts[i]); hi = std::max(hi, ts[i]); }
    if (lo > hi) return true;
    const double upper = c + 1 < ref.size() ? ref[c + 1] + kMargin : detail::kInf;
    return lo >= ref[c] - kMargin && hi <= upper;
}

// Clock-corrected chunk starts of a stream (tb.clock of each chunk's first stamp).
inline std::vector<double> clockedRefs(const StreamHeader& s, const Timebase& tb) {
    std::vector<double> r(s.chunks.size());
    for (std::size_t c = 0; c < r.size(); ++c) r[c] = tb.clock(s.chunks[c].ref, s.chunks[c].g);
    return r;
}

// Every raw timestamp of one stream, for dejitter and for clock-range splits.
inline std::vector<double> readAllStamps(const std::filesystem::path& path, const StreamHeader& s,
                                         const Timebase& tb, const std::atomic<bool>* cancel = nullptr) {
    const std::uint64_t nTotal = tb.limit ? std::min(s.count, *tb.limit) : s.count;
    std::vector<double> out((std::size_t)nTotal);
    InFile f;
    if (!f.open(path)) throw std::runtime_error(path.string() + ": cannot open the file");
    // Clock ranges are not assigned yet when this pass is what assigns them, and the
    // clock is only cheap once they are; such streams skip the check.
    std::vector<double> cref;
    if (!tb.needsPrepass()) cref = chunkStarts(clockedRefs(s, tb), s.chunks, s.tdiff);
    std::vector<std::uint8_t> raw;
    std::vector<double> tmp;
    Samples smp;
    double last = 0.0;
    for (std::size_t c = 0; c < s.chunks.size(); ++c) {
        const ChunkRef& ch = s.chunks[c];
        if (ch.g >= nTotal) break;
        if (cancel && cancel->load(std::memory_order_relaxed)) throw std::runtime_error("cancelled");
        const std::size_t n = (std::size_t)ch.n;
        try {
            if (!f.readAt(ch.off, (std::size_t)(ch.end - ch.off), raw)) throw Damaged("cut-off chunk");
            double newLast = last;
            decode(s, raw.data(), raw.size(), newLast, false, smp);
            if (!cref.empty()) {
                tmp.assign(smp.ts.begin(), smp.ts.begin() + (std::ptrdiff_t)smp.n);
                tb.clock(tmp.data(), ch.g, tmp.size());
                if (!plausible(tmp.data(), tmp.size(), cref, c))
                    throw Damaged("timestamps do not fit the chunks around it");
            }
            last = newLast;
        } catch (const Damaged&) {
            // Playback skips this chunk; deduced stamps keep the fits finite.
            smp.ts.resize(n);
            for (std::size_t i = 0; i < n; ++i) smp.ts[i] = last + s.tdiff * (double)(i + 1);
            smp.n = n;
            if (n) last = smp.ts[n - 1];
        }
        const std::size_t m = (std::size_t)std::min<std::uint64_t>(smp.n, nTotal - ch.g);
        std::copy(smp.ts.begin(), smp.ts.begin() + (std::ptrdiff_t)m, out.begin() + (std::ptrdiff_t)ch.g);
    }
    return out;
}

// Timestamp of the stream's first sample as pyxdf returns it. Dejitter applies only
// where it was fitted, that is, for streams being replayed.
inline std::optional<double> firstTime(const StreamHeader& s, const Timebase& tb) {
    if (!s.first) return std::nullopt;
    return tb.final(*s.first, 0);
}

// Where to start decoding for a window that begins at lo. ref holds the chunk start
// times; last0 is the previous raw timestamp at that chunk (NaN if unknown).
struct StartPlan { std::size_t chunk = 0; double last0 = 0.0; };
inline StartPlan planStart(const StreamHeader& s, const std::vector<double>& ref, double lo) {
    StartPlan p;
    if (s.chunks.empty()) return p;
    std::size_t c = 0;
    for (std::size_t i = ref.size(); i-- > 0;)
        if (std::isfinite(ref[i]) && ref[i] <= lo - kMargin) { c = i; break; }
    if (c && !s.chunks[c].firstExp) {
        // The first sample's timestamp is deduced from the previous chunk, so decode
        // back to a chunk that carries a stamp of its own.
        --c;
        while (c > 0 && !s.chunks[c].hasExp) --c;
    }
    p.chunk = c;
    p.last0 = c == 0 ? 0.0 : detail::kNaN;
    return p;
}

// ---------------------------------------------------------------------------
// Stream selection: a comma list of stream ids, "type:<pattern>", or name
// patterns with shell-style wildcards (* ? [seq] [!seq]), case-sensitive.
// ---------------------------------------------------------------------------
inline bool fnmatch(std::string_view pat, std::string_view s) {
    std::size_t p = 0, i = 0, starP = std::string_view::npos, starI = 0;
    auto classMatch = [&](std::size_t at, unsigned char c, std::size_t& next) -> int {   // 1 hit, 0 miss, -1 literal '['
        std::size_t j = at + 1;
        bool neg = false;
        if (j < pat.size() && pat[j] == '!') { neg = true; ++j; }
        const std::size_t first = j;
        bool hit = false;
        for (; j < pat.size() && (pat[j] != ']' || j == first); ++j) {
            if (j + 2 < pat.size() && pat[j + 1] == '-' && pat[j + 2] != ']') {
                if ((unsigned char)pat[j] <= c && c <= (unsigned char)pat[j + 2]) hit = true;
                j += 2;
            } else if ((unsigned char)pat[j] == c) {
                hit = true;
            }
        }
        if (j >= pat.size()) return -1;
        next = j + 1;
        return hit != neg ? 1 : 0;
    };
    while (i < s.size()) {
        bool ok = false;
        std::size_t next = p + 1;
        if (p < pat.size()) {
            if (pat[p] == '*') { starP = p++; starI = i; continue; }
            if (pat[p] == '?') ok = true;
            else if (pat[p] == '[') {
                const int r = classMatch(p, (unsigned char)s[i], next);
                ok = r < 0 ? s[i] == '[' : r == 1;
                if (r < 0) next = p + 1;
            } else ok = pat[p] == s[i];
        }
        if (ok) { p = next; ++i; continue; }
        if (starP == std::string_view::npos) return false;
        p = starP + 1;
        i = ++starI;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// Indices into streams, in the order the spec names them. Empty spec: all. On a
// spec item with no match, sets err and returns an empty list.
inline std::vector<std::size_t> selectStreams(const std::vector<StreamHeader>& streams,
                                              std::string_view spec, std::string& err) {
    std::vector<std::size_t> chosen;
    if (detail::trim(spec).empty()) {
        for (std::size_t i = 0; i < streams.size(); ++i) chosen.push_back(i);
        return chosen;
    }
    while (!spec.empty()) {
        const std::size_t comma = spec.find(',');
        const std::string_view item = detail::trim(spec.substr(0, comma));
        spec = comma == std::string_view::npos ? std::string_view() : spec.substr(comma + 1);
        if (item.empty()) continue;
        std::vector<std::size_t> hits;
        const bool digits = std::all_of(item.begin(), item.end(), [](char c) { return c >= '0' && c <= '9'; });
        long long id = -1;
        if (digits && detail::parseInt(item, id)) {
            for (std::size_t i = 0; i < streams.size(); ++i) if ((long long)streams[i].id == id) hits.push_back(i);
        } else if (item.size() >= 5 && (item[0] | 0x20) == 't' && (item[1] | 0x20) == 'y' &&
                   (item[2] | 0x20) == 'p' && (item[3] | 0x20) == 'e' && item[4] == ':') {
            for (std::size_t i = 0; i < streams.size(); ++i) if (fnmatch(item.substr(5), streams[i].type)) hits.push_back(i);
        } else {
            for (std::size_t i = 0; i < streams.size(); ++i) if (fnmatch(item, streams[i].name)) hits.push_back(i);
        }
        if (hits.empty()) { err = "no stream matches '" + std::string(item) + "' (see --list)"; return {}; }
        for (std::size_t h : hits) if (std::find(chosen.begin(), chosen.end(), h) == chosen.end()) chosen.push_back(h);
    }
    return chosen;
}

}  // namespace xdf
