#pragma once
// Cross-platform "magic" (mirrored) ring buffer.
// The same physical pages are mapped twice, back-to-back in virtual memory, so a
// read or write of up to bytes() starting anywhere in [0, bytes()) is always
// contiguous: no wrap branch, no split memcpy. Ideal for one-shot chunk writes
// and contiguous FFT-window / strided ImPlot reads from a high-rate LSL stream.
//
//   Linux  : memfd_create + double MAP_FIXED
//   macOS  : shm_open (unique, immediately unlinked) + double MAP_FIXED
//            (page size is 16 KiB on Apple Silicon; handled via sysconf)
//   Windows: VirtualAlloc2 placeholder + two MapViewOfFile3 (Win10 1803+).
//            Build with _WIN32_WINNT >= 0x0A00 and link onecore.lib.
//            (MinGW may lack the VirtualAlloc2 import lib; prefer MSVC/clang-cl.)

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>      // std::gcd
#include <stdexcept>

#if defined(_WIN32)
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX        // keep windows.h from defining min/max macros
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <memoryapi.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <fcntl.h>
  #if defined(__linux__)
    #include <sys/syscall.h>
  #endif
#endif

class MagicRingBuffer {
public:
    MagicRingBuffer() = default;
    ~MagicRingBuffer() { release(); }
    MagicRingBuffer(const MagicRingBuffer&) = delete;
    MagicRingBuffer& operator=(const MagicRingBuffer&) = delete;

    // Allocate a mirrored region of at least `requested` bytes, rounded up to the
    // mapping granularity. Afterwards: data()[i] aliases data()[i + bytes()].
    void allocate(std::size_t requested) {
        release();
        bytes_ = roundUp(requested, granularity());
#if defined(_WIN32)
        allocWindows();
#else
        allocPosix();
#endif
    }

    void*       data()        { return base_; }
    const void* data() const  { return base_; }
    std::size_t bytes() const { return bytes_; }   // length of ONE copy

    static std::size_t granularity() {
#if defined(_WIN32)
        SYSTEM_INFO si; GetSystemInfo(&si);
        return si.dwAllocationGranularity;          // 64 KiB
#else
        return (std::size_t)sysconf(_SC_PAGESIZE);  // 4 KiB, or 16 KiB on Apple Silicon
#endif
    }

private:
    void*        base_  = nullptr;
    std::size_t  bytes_ = 0;
#if defined(_WIN32)
    HANDLE       section_ = nullptr;
#endif

    static std::size_t roundUp(std::size_t v, std::size_t m) { return (v + m - 1) / m * m; }

#if defined(_WIN32)
    void allocWindows() {
        section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            (DWORD)((std::uint64_t)bytes_ >> 32), (DWORD)(bytes_ & 0xFFFFFFFFu), nullptr);
        if (!section_) throw std::runtime_error("CreateFileMapping failed");

        for (int attempt = 0; attempt < 32; ++attempt) {     // retry: placeholder race
            void* ph = VirtualAlloc2(nullptr, nullptr, 2 * bytes_,
                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
            if (!ph) continue;
            if (!VirtualFree(ph, bytes_, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                VirtualFree(ph, 0, MEM_RELEASE);
                continue;
            }
            void* v1 = MapViewOfFile3(section_, GetCurrentProcess(), ph, 0, bytes_,
                MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
            void* v2 = MapViewOfFile3(section_, GetCurrentProcess(), (char*)ph + bytes_, 0,
                bytes_, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
            if (v1 && v2) { base_ = ph; return; }
            if (v1) UnmapViewOfFileEx(v1, 0);
            if (v2) UnmapViewOfFileEx(v2, 0);
            VirtualFree(ph, 0, MEM_RELEASE);
        }
        CloseHandle(section_); section_ = nullptr;
        throw std::runtime_error("magic buffer: could not place two adjacent views");
    }
#else
    void allocPosix() {
        int fd = -1;
#if defined(__linux__)
        fd = (int)syscall(SYS_memfd_create, "lsl_ring", 0u);
        if (fd < 0) throw std::runtime_error("memfd_create failed");
#else // macOS / BSD: named object, unlinked immediately so it is effectively anonymous
        char name[64];
        static std::atomic<unsigned> counter{0};
        std::snprintf(name, sizeof(name), "/lslrb-%d-%u", (int)getpid(), counter++);
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) throw std::runtime_error("shm_open failed");
        shm_unlink(name);
#endif
        if (ftruncate(fd, (off_t)bytes_) != 0) { ::close(fd); throw std::runtime_error("ftruncate failed"); }

        for (int attempt = 0; attempt < 32; ++attempt) {
            void* reserve = mmap(nullptr, 2 * bytes_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (reserve == MAP_FAILED) continue;
            void* v1 = mmap(reserve, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
            void* v2 = mmap((char*)reserve + bytes_, bytes_, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_FIXED, fd, 0);
            if (v1 == reserve && v2 == (char*)reserve + bytes_) { base_ = reserve; ::close(fd); return; }
            munmap(reserve, 2 * bytes_);
        }
        ::close(fd);
        throw std::runtime_error("magic buffer: could not place two adjacent maps");
    }
#endif

    void release() {
        if (!base_) return;
#if defined(_WIN32)
        UnmapViewOfFileEx(base_, 0);
        UnmapViewOfFileEx((char*)base_ + bytes_, 0);
        if (section_) { CloseHandle(section_); section_ = nullptr; }
#else
        munmap(base_, 2 * bytes_);
#endif
        base_ = nullptr; bytes_ = 0;
    }
};

// ---------------------------------------------------------------------------
// Single-producer / multi-reader interleaved float ring built on the magic buffer.
// Layout (LSL "multiplexed" order): [s0c0 s0c1 ... s0c(K-1) | s1c0 ...].
// Producer writes a whole chunk with ONE memcpy; the mirror handles the wrap.
// Readers grab the most-recent window; for a live plot, tearing is invisible.
// For FFT continuity, re-read head() after copying and retry if it advanced
// past your window start (seqlock-lite).
// ---------------------------------------------------------------------------
class InterleavedRing {
public:
    void init(int channels, std::size_t min_history_samples) {
        channels_ = channels;
        const std::size_t elem = (std::size_t)channels * sizeof(float);   // bytes per sample
        // Round to a whole number of samples AND a mapping unit, or the physical
        // mirror seam drifts out of phase with the sample wrap.
        const std::size_t unit = lcm(MagicRingBuffer::granularity(), elem);
        mem_.allocate(roundUp(min_history_samples * elem, unit));
        cap_samples_ = mem_.bytes() / elem;        // exact: bytes() is a multiple of elem
        head_.store(0, std::memory_order_relaxed);
    }

    int           channels() const { return channels_; }
    std::size_t   capacity() const { return cap_samples_; }
    std::uint64_t head() const     { return head_.load(std::memory_order_acquire); }
    float*        base()           { return (float*)mem_.data(); }

    // PRODUCER thread only. n <= capacity().
    void write(const float* interleaved, std::size_t n) {
        std::uint64_t h = head_.load(std::memory_order_relaxed);
        std::size_t off = (std::size_t)(h % cap_samples_);
        std::memcpy(base() + off * channels_, interleaved, n * channels_ * sizeof(float));
        head_.store(h + n, std::memory_order_release);
    }

    // READER: pointer to the start of the most-recent `window` samples (clamped).
    // Stride between successive samples of one channel is channels() floats; the
    // mirror guarantees [ptr, ptr + window*channels) stays mapped across the wrap.
    const float* recent(std::size_t window, std::size_t& count, std::uint64_t& start) const {
        std::uint64_t h = head_.load(std::memory_order_acquire);
        std::size_t avail = (h < cap_samples_) ? (std::size_t)h : cap_samples_;
        count = std::min(window, avail);
        start = h - count;
        std::size_t off = (std::size_t)(start % cap_samples_);
        return (const float*)mem_.data() + off * channels_;
    }

    // READER: pointer to the sample at absolute index `startAbs`. Caller guarantees
    // [startAbs, startAbs+n) is resident (startAbs >= head-capacity, startAbs+n <= head)
    // with n <= capacity(); the mirror keeps that span contiguous. Used by the STFT to
    // read overlapping windows at exact hop positions, not just the live edge.
    const float* windowAt(std::uint64_t startAbs) const {
        std::size_t off = (std::size_t)(startAbs % cap_samples_);
        return (const float*)mem_.data() + off * channels_;
    }

private:
    MagicRingBuffer            mem_;
    int                        channels_ = 0;
    std::size_t                cap_samples_ = 0;
    std::atomic<std::uint64_t> head_{0};

    static std::size_t roundUp(std::size_t v, std::size_t m) { return (v + m - 1) / m * m; }
    static std::size_t lcm(std::size_t a, std::size_t b) { return a / std::gcd(a, b) * b; }
};
