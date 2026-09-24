#include "ExrFast.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <tmmintrin.h>
#define EXRFAST_X86 1
#define EXRFAST_TARGET_SSSE3
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <tmmintrin.h>
#define EXRFAST_X86 1
#define EXRFAST_TARGET_SSSE3 __attribute__((target("ssse3")))
#endif

namespace exrfast {

namespace {

// ---------------------------------------------------------------------------
// Interleave kernel

#if EXRFAST_X86
bool hasSsse3() {
    static const bool v = [] {
#if defined(_MSC_VER)
        int r[4] = { 0, 0, 0, 0 };
        __cpuid(r, 1);
        return (r[2] & (1 << 9)) != 0;
#else
        return __builtin_cpu_supports("ssse3") != 0;
#endif
    }();
    return v;
}

// pshufb control bytes: output vector k (of the three that hold 8 pixels of RGB
// = 24 halves) takes, in 16-bit lane j, element i = 8k+j of the r0 g0 b0 r1 g1
// b1 ... sequence, which is pixel i/3 of plane i%3. One mask per (k, plane): the
// plane's own lanes select bytes 2p/2p+1, every other lane is zeroed (0x80) so the
// three shuffled planes OR together.
struct Masks {
    alignas(16) unsigned char m[3][3][16]; // [k][plane][byte]
};
constexpr Masks makeMasks() {
    Masks M{};
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 3; ++c)
            for (int j = 0; j < 8; ++j) {
                const int i = k * 8 + j;
                const int p = i / 3, ch = i % 3;
                if (ch == c) {
                    M.m[k][c][2 * j] = (unsigned char)(2 * p);
                    M.m[k][c][2 * j + 1] = (unsigned char)(2 * p + 1);
                } else {
                    M.m[k][c][2 * j] = 0x80;
                    M.m[k][c][2 * j + 1] = 0x80;
                }
            }
    return M;
}
constexpr Masks kMasks = makeMasks();

EXRFAST_TARGET_SSSE3
void interleaveSsse3(const uint16_t* r, const uint16_t* g, const uint16_t* b, uint16_t* dst,
                     size_t n) {
    const __m128i mR0 = _mm_load_si128((const __m128i*)kMasks.m[0][0]);
    const __m128i mG0 = _mm_load_si128((const __m128i*)kMasks.m[0][1]);
    const __m128i mB0 = _mm_load_si128((const __m128i*)kMasks.m[0][2]);
    const __m128i mR1 = _mm_load_si128((const __m128i*)kMasks.m[1][0]);
    const __m128i mG1 = _mm_load_si128((const __m128i*)kMasks.m[1][1]);
    const __m128i mB1 = _mm_load_si128((const __m128i*)kMasks.m[1][2]);
    const __m128i mR2 = _mm_load_si128((const __m128i*)kMasks.m[2][0]);
    const __m128i mG2 = _mm_load_si128((const __m128i*)kMasks.m[2][1]);
    const __m128i mB2 = _mm_load_si128((const __m128i*)kMasks.m[2][2]);
    for (; n >= 8; n -= 8, r += 8, g += 8, b += 8, dst += 24) {
        const __m128i R = _mm_loadu_si128((const __m128i*)r);
        const __m128i G = _mm_loadu_si128((const __m128i*)g);
        const __m128i B = _mm_loadu_si128((const __m128i*)b);
        _mm_storeu_si128((__m128i*)(dst),
                         _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(R, mR0), _mm_shuffle_epi8(G, mG0)),
                                      _mm_shuffle_epi8(B, mB0)));
        _mm_storeu_si128((__m128i*)(dst + 8),
                         _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(R, mR1), _mm_shuffle_epi8(G, mG1)),
                                      _mm_shuffle_epi8(B, mB1)));
        _mm_storeu_si128((__m128i*)(dst + 16),
                         _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(R, mR2), _mm_shuffle_epi8(G, mG2)),
                                      _mm_shuffle_epi8(B, mB2)));
    }
    for (; n; --n, dst += 3) {
        dst[0] = *r++;
        dst[1] = *g++;
        dst[2] = *b++;
    }
}
#endif // EXRFAST_X86

void interleaveScalar(const uint16_t* r, const uint16_t* g, const uint16_t* b, uint16_t* dst,
                      size_t n) {
    for (; n; --n, dst += 3) {
        dst[0] = *r++;
        dst[1] = *g++;
        dst[2] = *b++;
    }
}

// ---------------------------------------------------------------------------
// Header parsing. Bounds-checked cursor over a byte buffer; any read past the end
// or any value that does not make sense clears `ok`, and the caller falls back.

struct Cursor {
    const unsigned char* p;
    size_t n;
    size_t pos = 0;
    bool ok = true;

    bool need(size_t k) {
        if (!ok || pos + k > n) {
            ok = false;
            return false;
        }
        return true;
    }
    uint8_t u8() { return need(1) ? p[pos++] : 0; }
    int32_t i32() {
        if (!need(4)) return 0;
        int32_t v;
        std::memcpy(&v, p + pos, 4);
        pos += 4;
        return v;
    }
    // Null-terminated string of at most `maxLen` characters.
    bool str(std::string& out, size_t maxLen = 255) {
        if (!ok) return false;
        const size_t s = pos;
        while (pos < n && p[pos] != 0) {
            if (pos - s >= maxLen) {
                ok = false;
                return false;
            }
            ++pos;
        }
        if (pos >= n) {
            ok = false;
            return false;
        }
        out.assign((const char*)p + s, pos - s);
        ++pos;
        return true;
    }
    bool skip(size_t k) {
        if (!need(k)) return false;
        pos += k;
        return true;
    }
};

enum PixelType { kUint = 0, kHalf = 1, kFloat = 2 };

struct Channel {
    std::string name;
    int type = -1;
    int xSampling = 1, ySampling = 1;
};

struct PartHeader {
    std::vector<Channel> channels; // file order (OpenEXR writes them sorted by name)
    int compression = -1;
    int lineOrder = 0;
    int32_t dataMin[2] = { 0, 0 }, dataMax[2] = { -1, -1 };
    int32_t dispMin[2] = { 0, 0 }, dispMax[2] = { -1, -1 };
    bool haveChannels = false, haveCompression = false, haveData = false, haveDisp = false;
    bool tiled = false;      // carries a tiledesc attribute
    int64_t chunkCount = -1; // multi-part only (required there)
    std::string type;        // multi-part only ("scanlineimage", "tiledimage", ...)
};

// Parse one header starting at the cursor. Returns false (cursor ok) if the header
// is empty -- the terminator of a multi-part header list -- and sets cursor.ok to
// false on malformed input.
bool parseHeader(Cursor& c, PartHeader& h) {
    if (!c.need(1)) return false;
    if (c.p[c.pos] == 0) {
        ++c.pos;
        return false; // empty header
    }
    std::string name, type;
    for (;;) {
        if (!c.str(name)) return false;
        if (name.empty()) return true; // end of this header
        if (!c.str(type)) return false;
        const int32_t size = c.i32();
        if (!c.ok || size < 0 || !c.need((size_t)size)) {
            c.ok = false;
            return false;
        }
        const size_t end = c.pos + (size_t)size;
        if (name == "channels" && type == "chlist") {
            Cursor s{ c.p + c.pos, (size_t)size };
            for (;;) {
                if (!s.need(1)) break;
                if (s.p[s.pos] == 0) {
                    ++s.pos;
                    break;
                }
                Channel ch;
                if (!s.str(ch.name)) break;
                ch.type = s.i32();
                s.u8();  // pLinear
                s.skip(3);
                ch.xSampling = s.i32();
                ch.ySampling = s.i32();
                if (!s.ok) break;
                h.channels.push_back(std::move(ch));
            }
            if (!s.ok) {
                c.ok = false;
                return false;
            }
            h.haveChannels = true;
        } else if (name == "compression" && type == "compression" && size == 1) {
            h.compression = c.u8();
            h.haveCompression = true;
        } else if (name == "dataWindow" && type == "box2i" && size == 16) {
            h.dataMin[0] = c.i32(); h.dataMin[1] = c.i32();
            h.dataMax[0] = c.i32(); h.dataMax[1] = c.i32();
            h.haveData = true;
        } else if (name == "displayWindow" && type == "box2i" && size == 16) {
            h.dispMin[0] = c.i32(); h.dispMin[1] = c.i32();
            h.dispMax[0] = c.i32(); h.dispMax[1] = c.i32();
            h.haveDisp = true;
        } else if (name == "lineOrder" && type == "lineOrder" && size == 1) {
            h.lineOrder = c.u8();
        } else if (name == "chunkCount" && type == "int" && size == 4) {
            h.chunkCount = c.i32();
        } else if (name == "type" && type == "string") {
            h.type.assign((const char*)c.p + c.pos, (size_t)size);
        } else if (type == "tiledesc") {
            h.tiled = true;
        }
        c.pos = end; // whatever was consumed above, land exactly after the value
        if (!c.ok) return false;
    }
}

size_t bytesPerSample(int t) {
    switch (t) {
    case kHalf:  return 2;
    case kUint:  return 4;
    case kFloat: return 4;
    default:    return 0;
    }
}

constexpr size_t kHeaderBytes = 64 * 1024;       // first read; grown if the header runs past it
constexpr size_t kHeaderMax = 16u << 20;          // give up on a header this size
constexpr size_t kBlockBytes = 1u << 20;          // streaming read size (whole chunks)

struct File {
    FILE* f = nullptr;
    ~File() {
        if (f) std::fclose(f);
    }
};

bool seekTo(FILE* f, int64_t off) {
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

FILE* openRead(const std::string& path) {
#ifdef _WIN32
    // Wide open so a UTF-8 path survives; 'S' is the sequential-scan hint, which is
    // exactly the access pattern below.
    return _wfopen(std::filesystem::u8path(path).c_str(), L"rbS");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}


} // namespace

// ---------------------------------------------------------------------------

bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("JPLAY_EXR_NOFAST");
        return !(v && v[0] && v[0] != '0');
    }();
    return on;
}

void interleaveRgbHalf(const uint16_t* r, const uint16_t* g, const uint16_t* b, uint16_t* dst,
                       size_t n) {
#if EXRFAST_X86
    if (hasSsse3()) {
        interleaveSsse3(r, g, b, dst, n);
        return;
    }
#endif
    interleaveScalar(r, g, b, dst, n);
}

bool readRgbHalf(const std::string& path, int part, const std::string (&rgb)[3],
                 Frame::HalfBuffer& out, int& dispW, int& dispH) {
    File file;
    file.f = openRead(path);
    if (!file.f) return false;
    FILE* f = file.f;
    std::setvbuf(f, nullptr, _IONBF, 0); // every fread below is one large read; no CRT buffer

    // Headers. Read a first block; a header that runs past it (a file heavy with
    // metadata) is re-read at double the size.
    std::vector<unsigned char> hdr(kHeaderBytes);
    std::vector<PartHeader> parts;
    bool multipart = false;
    size_t headerEnd = 0;
    for (;;) {
        if (!seekTo(f, 0)) return false;
        const size_t got = std::fread(hdr.data(), 1, hdr.size(), f);
        Cursor c{ hdr.data(), got };
        const int32_t magic = c.i32();
        const int32_t version = c.i32();
        if (!c.ok || magic != 20000630) return false;
        if ((version & 0xff) != 2) return false;
        if (version & 0x200) return false;  // single-part tiled
        if (version & 0x800) return false;  // deep data
        multipart = (version & 0x1000) != 0;

        parts.clear();
        bool truncated = false;
        if (multipart) {
            for (;;) {
                PartHeader h;
                const bool more = parseHeader(c, h);
                if (!c.ok) {
                    truncated = true;
                    break;
                }
                if (!more) break; // the empty terminating header
                parts.push_back(std::move(h));
                if (parts.size() > 256) return false;
            }
        } else {
            PartHeader h;
            const bool more = parseHeader(c, h);
            if (!c.ok) truncated = true;
            else if (!more) return false; // a single-part file cannot start with an empty header
            else parts.push_back(std::move(h));
        }
        if (!truncated) {
            headerEnd = c.pos;
            break;
        }
        // Ran off the end of what was read: either the file is short (then the
        // next read comes back the same size and we stop) or the header is long.
        if (got < hdr.size() || hdr.size() >= kHeaderMax) return false;
        hdr.resize(hdr.size() * 2);
    }

    if (part < 0 || part >= (int)parts.size()) return false;
    const PartHeader& h = parts[(size_t)part];
    if (!h.haveChannels || !h.haveCompression || !h.haveData || !h.haveDisp) return false;
    if (h.compression != 0) return false; // NO_COMPRESSION only
    if (h.tiled) return false;
    if (multipart && h.type != "scanlineimage") return false;
    if (!multipart && !h.type.empty() && h.type != "scanlineimage") return false;

    dispW = h.dispMax[0] - h.dispMin[0] + 1;
    dispH = h.dispMax[1] - h.dispMin[1] + 1;
    const int64_t dataW = (int64_t)h.dataMax[0] - h.dataMin[0] + 1;
    const int64_t dataH = (int64_t)h.dataMax[1] - h.dataMin[1] + 1;
    if (dispW <= 0 || dispH <= 0 || dataW <= 0 || dataH <= 0) return false;
    if ((int64_t)dispW * dispH > (int64_t)1 << 31) return false;

    // Channel layout within one scanline chunk: each channel's full row, in file
    // order. Any subsampled channel would make that layout row-dependent.
    // A name repeated across the slots resolves to the same offset, which the
    // interleave below reads three times over: grayscale at no extra cost.
    size_t rowBytes = 0;
    size_t off[3] = {};
    bool have[3] = {};
    for (const Channel& ch : h.channels) {
        if (ch.xSampling != 1 || ch.ySampling != 1) return false;
        const size_t bps = bytesPerSample(ch.type);
        if (!bps) return false;
        if (ch.type == kHalf)
            for (int k = 0; k < 3; ++k)
                if (ch.name == rgb[k]) { off[k] = rowBytes; have[k] = true; }
        rowBytes += (size_t)dataW * bps;
    }
    if (!have[0] || !have[1] || !have[2]) return false;
    const size_t offR = off[0], offG = off[1], offB = off[2];

    // Chunk table: one 8-byte offset per scanline (an uncompressed chunk is one
    // line), following every part's header, one table per part in part order.
    int64_t tableStart = (int64_t)headerEnd;
    int64_t chunkCount = dataH;
    if (multipart) {
        for (int q = 0; q < (int)parts.size(); ++q) {
            if (parts[(size_t)q].chunkCount < 0) return false;
            if (q < part) tableStart += parts[(size_t)q].chunkCount * 8;
        }
        chunkCount = h.chunkCount;
        if (chunkCount != dataH) return false;
    }
    std::vector<int64_t> offsets((size_t)chunkCount);
    if (!seekTo(f, tableStart)) return false;
    if (std::fread(offsets.data(), 8, offsets.size(), f) != offsets.size()) return false;

    // Stream only a layout that is one contiguous run of equal-sized chunks -- what
    // every writer produces for an uncompressed scanline part. Chunks may sit in
    // the run in any order (line order is read off each chunk's own header).
    const size_t chunkBytes = (multipart ? 12 : 8) + rowBytes;
    std::vector<int64_t> sorted(offsets);
    std::sort(sorted.begin(), sorted.end());
    const int64_t base = sorted.front();
    if (base < tableStart + (int64_t)offsets.size() * 8) return false;
    for (size_t i = 0; i < sorted.size(); ++i)
        if (sorted[i] != base + (int64_t)i * (int64_t)chunkBytes) return false;

    // The part of the data window that lands on the display window.
    const int x0 = std::max(h.dispMin[0], h.dataMin[0]);
    const int x1 = std::min(h.dispMax[0], h.dataMax[0]);
    const int y0 = std::max(h.dispMin[1], h.dataMin[1]);
    const int y1 = std::min(h.dispMax[1], h.dataMax[1]);
    const bool covers = h.dataMin[0] <= h.dispMin[0] && h.dataMin[1] <= h.dispMin[1] &&
                        h.dataMax[0] >= h.dispMax[0] && h.dataMax[1] >= h.dispMax[1];
    const size_t npx = (size_t)dispW * dispH;
    // See ExrSequenceSource::readFrame: only a border the data does not cover
    // needs clearing; otherwise every element is written below.
    if (covers)
        out.resize(npx * 3);
    else
        out.assign(npx * 3, Imath::half(0.0f));
    if (x1 < x0 || y1 < y0)
        return true; // no overlap: the frame is all border
    const size_t copyPx = (size_t)(x1 - x0 + 1);
    const size_t srcCol = (size_t)(x0 - h.dataMin[0]) * 2;

    // One scanline chunk: header checks, then the row's R/G/B planes interleaved
    // into place. Rows outside the display window are skipped.
    uint16_t* const dst0 = reinterpret_cast<uint16_t*>(out.data());
    auto processChunk = [&](const unsigned char* q) -> bool {
        int32_t v;
        if (multipart) {
            std::memcpy(&v, q, 4);
            q += 4;
            if (v != part) return false;
        }
        int32_t y, size;
        std::memcpy(&y, q, 4);
        std::memcpy(&size, q + 4, 4);
        q += 8;
        if (size != (int32_t)rowBytes || y < h.dataMin[1] || y > h.dataMax[1]) return false;
        if (y < y0 || y > y1) return true; // outside the display window
        uint16_t* dst = dst0 + ((size_t)(y - h.dispMin[1]) * dispW + (size_t)(x0 - h.dispMin[0])) * 3;
        interleaveRgbHalf(reinterpret_cast<const uint16_t*>(q + offR + srcCol),
                          reinterpret_cast<const uint16_t*>(q + offG + srcCol),
                          reinterpret_cast<const uint16_t*>(q + offB + srcCol), dst, copyPx);
        return true;
    };

    // Read the run in large blocks of whole chunks and interleave each row as it
    // arrives. The block is per thread and sized once, so after the first frame
    // it costs no allocation and no faults; at 1 MB it also stays in cache for
    // the interleave that reads it back, with eight decode threads streaming at
    // once (8 MB blocks spilled out of L3 and halved the parallel throughput).
    // Mapping the file instead was measured too, with and without prefetching
    // the view, and lost: 17-20 ms per 4K frame against 9 ms here, the soft
    // faults on 13k mapped pages costing more than the copy they save.
    const size_t chunksPerBlock = std::max<size_t>(1, kBlockBytes / chunkBytes);
    thread_local std::vector<unsigned char> block;
    block.resize(chunksPerBlock * chunkBytes);
    if (!seekTo(f, base)) return false;

    int64_t remaining = chunkCount;
    while (remaining > 0) {
        const size_t take = (size_t)std::min<int64_t>(remaining, (int64_t)chunksPerBlock);
        const size_t want = take * chunkBytes;
        if (std::fread(block.data(), 1, want, f) != want) return false;
        const unsigned char* p = block.data();
        for (size_t i = 0; i < take; ++i, p += chunkBytes)
            if (!processChunk(p)) return false;
        remaining -= (int64_t)take;
    }
    return true;
}

} // namespace exrfast
