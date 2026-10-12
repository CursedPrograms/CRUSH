// CRUSH — a from-scratch compressor/archiver in C++.
//
// The compression is ours: a binary range coder (arithmetic coding) driven by a context-mixing
// model with a match model. No zlib, no libraries — just the standard library and file I/O.
//
//   v3 (written by this version): adaptive-rate counters, order 0-6 contexts, a match model,
//      mixer weights chosen by match state, and two APM / SSE stages refining the final guess.
//   v2 (still read): order 1-6 contexts + match model, one mixer.
//   v1 is the Python reference (crush.py), order-1 only.
//
// Build:   g++ -O2 -std=c++17 -static -municode -o crush.exe crush.cpp   (Windows; drop -municode elsewhere)
//          cl /O2 /std:c++17 /EHsc /MT crush.cpp                          (MSVC)
// Use:     crush a out.crush file_or_folder ...   create (folders are added recursively)
//          crush x out.crush [dest_dir]          extract
//          crush e out.crush                     extract into a folder named after the archive
//          crush l out.crush                     list + ratios
//          crush t out.crush                     test integrity (CRC32)

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <memory>

using std::uint8_t; using std::uint16_t; using std::uint32_t; using std::uint64_t;
using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;

// ── the range coder ──────────────────────────────────────────────────────────
static const int      PBITS   = 12;
static const uint32_t PSCALE  = 1u << PBITS;   // 4096
static const uint32_t PHALF   = PSCALE >> 1;   // 2048
static const int      ADAPT   = 5;

struct RangeEncoder {
    uint32_t x1 = 0, x2 = 0xFFFFFFFFu;
    Bytes out;
    void encode(int bit, uint32_t p) {           // p = P(bit==1), 12-bit, [1,4095]
        uint32_t xmid = x1 + ((x2 - x1) >> PBITS) * p;
        if (bit) x2 = xmid; else x1 = xmid + 1;
        while (((x1 ^ x2) & 0xFF000000u) == 0) {
            out.push_back(uint8_t(x2 >> 24));
            x1 <<= 8;
            x2 = (x2 << 8) | 0xFF;
        }
    }
    Bytes finish() {
        for (int i = 0; i < 4; i++) { out.push_back(uint8_t(x1 >> 24)); x1 <<= 8; }
        return out;
    }
};

struct RangeDecoder {
    uint32_t x1 = 0, x2 = 0xFFFFFFFFu, x = 0;
    const Bytes& data; size_t pos = 0;
    explicit RangeDecoder(const Bytes& d) : data(d) {
        for (int i = 0; i < 4; i++) x = (x << 8) | get();
    }
    uint32_t get() { return pos < data.size() ? data[pos++] : 0; }
    int decode(uint32_t p) {
        uint32_t xmid = x1 + ((x2 - x1) >> PBITS) * p;
        int bit;
        if (x <= xmid) { bit = 1; x2 = xmid; } else { bit = 0; x1 = xmid + 1; }
        while (((x1 ^ x2) & 0xFF000000u) == 0) {
            x1 <<= 8;
            x2 = (x2 << 8) | 0xFF;
            x  = (x << 8) | get();
        }
        return bit;
    }
};

// ── logistic helpers ─────────────────────────────────────────────────────────
static int stretch_tbl[4096];
static int squash(int d) {
    static const int t[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,
        1546,2047,2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
    if (d >  2047) return 4095;
    if (d < -2047) return 0;
    int w = d & 127;
    d = (d >> 7) + 16;
    return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}
static void mix_init() {
    int pi = 0;
    for (int x = -2047; x <= 2047; ++x) { int v = squash(x); for (; pi <= v; ++pi) stretch_tbl[pi] = x; }
    for (; pi < 4096; ++pi) stretch_tbl[pi] = 2047;
}

// ── v2 model (kept exactly as it was, so v2 archives still decode) ───────────
namespace v2 {
static const int NMODEL = 5;
static const int ORDERS[NMODEL] = {1, 2, 3, 4, 6};   // context lengths in bytes
static const int NIN = NMODEL + 1;                    // context models + 1 match model
static const int TBITS = 22;                          // 4M slots per context model
static const uint32_t TMASK = (1u << TBITS) - 1;
static const int MHBITS = 22;                         // match-model hash table
static const uint32_t MHMASK = (1u << MHBITS) - 1;
static const int MINMATCH = 6;                        // bytes that must agree to start a match

struct CM {
    std::vector<uint16_t> tbl[NMODEL];
    std::vector<uint32_t> mhash;        // hash(last MINMATCH bytes) -> position
    std::vector<int32_t>  w;            // 256 mixer-contexts * NIN weights
    uint8_t  hist[8] = {0};
    uint32_t h[NMODEL], idx[NMODEL];
    int      st[NIN];
    int      mixctx = 0;
    const Bytes* buf = nullptr; size_t pos = 0;
    uint32_t matchPtr = 0; int matchLen = 0; uint8_t mmByte = 0; bool mmAligned = false; int depth = 0;

    CM() : mhash(size_t(1) << MHBITS, 0), w(256 * NIN, (1 << 16) / NIN) {
        for (int m = 0; m < NMODEL; ++m) tbl[m].assign(size_t(1) << TBITS, uint16_t(PHALF));
    }

    void begin_byte(const Bytes* b, size_t p) {
        buf = b; pos = p; depth = 0;
        for (int m = 0; m < NMODEL; ++m) {
            uint32_t hh = 0x811c9dc5u;
            for (int i = 0; i < ORDERS[m]; ++i) hh = (hh ^ (hist[i] + 1)) * 0x6f4a7c13u;
            h[m] = hh + (uint32_t)(m * 0x9e3779b1u);
        }
        if (pos >= (size_t)MINMATCH) {
            uint32_t hh = 0x811c9dc5u;
            for (int i = 0; i < MINMATCH; ++i) hh = (hh ^ (*buf)[pos - 1 - i]) * 0x6f4a7c13u;
            hh &= MHMASK;
            if (matchLen == 0) {
                uint32_t cand = mhash[hh];
                if (cand) { matchPtr = cand; matchLen = 1; }
            }
            mhash[hh] = (uint32_t)pos;
        }
        mmAligned = (matchLen > 0 && matchPtr < pos);
        mmByte = mmAligned ? (*buf)[matchPtr] : 0;
    }

    int predict(uint32_t node) {
        int64_t dot = 0;
        int32_t* ww = &w[mixctx * NIN];
        for (int m = 0; m < NMODEL; ++m) {
            uint32_t id = (h[m] + node * 2654435761u) & TMASK;
            idx[m] = id;
            st[m]  = stretch_tbl[tbl[m][id]];
            dot += (int64_t)ww[m] * st[m];
        }
        int stm = 0;
        if (mmAligned) {
            int expbit = (mmByte >> (7 - depth)) & 1;
            int s = 400 + matchLen * 64; if (s > 2047) s = 2047;
            stm = expbit ? s : -s;
        }
        st[NMODEL] = stm;
        dot += (int64_t)ww[NMODEL] * stm;
        int pr = squash((int)(dot >> 16));
        if (pr < 1) pr = 1; if (pr > 4095) pr = 4095;
        return pr;
    }

    void learn(int bit, int pr) {
        int err = (bit << 12) - pr;
        int32_t* ww = &w[mixctx * NIN];
        for (int m = 0; m < NIN; ++m) {
            int nw = ww[m] + ((st[m] * err) >> 10);
            if (nw >  (1 << 22)) nw =  (1 << 22);
            if (nw < -(1 << 22)) nw = -(1 << 22);
            ww[m] = nw;
        }
        for (int m = 0; m < NMODEL; ++m) {
            int p = tbl[m][idx[m]];
            p += bit ? ((PSCALE - p) >> ADAPT) : -(p >> ADAPT);
            tbl[m][idx[m]] = uint16_t(p);
        }
        if (mmAligned && bit != ((mmByte >> (7 - depth)) & 1)) mmAligned = false;
        depth++;
    }

    void end_byte(uint8_t b) {
        if (matchLen > 0) {
            if (mmByte == b) { matchPtr++; if (matchLen < 65535) matchLen++; }
            else matchLen = 0;
        }
        for (int i = 7; i > 0; --i) hist[i] = hist[i - 1];
        hist[0] = b;
        mixctx = b;
    }
};
}  // namespace v2

// ── v3 model ─────────────────────────────────────────────────────────────────
namespace v3 {
static const int NMODEL = 6;
static const int ORDERS[NMODEL] = {0, 1, 2, 3, 4, 6};
static const int NIN = NMODEL + 2;                    // contexts + match model + bias
static const int TBITS = 22;
static const uint32_t TMASK = (1u << TBITS) - 1;
static const int MHBITS = 22;
static const uint32_t MHMASK = (1u << MHBITS) - 1;
static const int MINMATCH = 6;
static const int LIMIT = 12;                          // counters never slow below ~1/14: they keep up

// Counters: a 22-bit probability and a 10-bit count in one uint32. A new context learns fast
// (rate 1/(n+1.5)); one that has seen a lot settles down (rate 1/(LIMIT+1.5)).
static int recip[1024];
static void counters_init() { for (int n = 0; n < 1024; ++n) recip[n] = int(65536.0 / (n + 1.5)); }
static inline int counter_p12(uint32_t c) { return int(c >> 20); }          // 12-bit probability
static inline void counter_update(uint32_t& c, int bit) {
    uint32_t n = c & 1023, p = c >> 10;                                        // p: 22-bit
    int target = bit ? (1 << 22) - 1 : 0;
    p += uint32_t(((int64_t)(target - (int)p) * recip[n]) >> 16);
    if (n < (uint32_t)LIMIT) ++n;
    c = (p << 10) | n;
}

// APM / SSE: refines a probability given a small context, by interpolating between 33
// buckets along the stretch axis; each bucket learns what the probability really is there.
struct APM {
    std::vector<uint16_t> t;
    int idx = 0;
    explicit APM(int contexts) : t(size_t(contexts) * 33) {
        for (int c = 0; c < contexts; ++c)
            for (int j = 0; j < 33; ++j) t[size_t(c) * 33 + j] = uint16_t(squash((j - 16) * 128) * 16);
    }
    int refine(int pr, int ctx) {
        int s = stretch_tbl[pr] + 2048;               // 0 .. 4095
        int lo = s >> 7, w = s & 127;
        idx = ctx * 33 + lo;
        int p = (t[idx] * (128 - w) + t[idx + 1] * w) >> 11;
        if (w >= 64) idx++;                           // the nearer bucket learns
        return std::min(4095, std::max(1, p));
    }
    void learn(int bit, int rate = 6) {
        int target = bit ? 65535 : 0;
        t[idx] = uint16_t(t[idx] + ((target - int(t[idx])) >> rate));
    }
};

struct CM {
    std::vector<uint32_t> tbl[NMODEL];
    std::vector<uint32_t> mhash;
    std::vector<int32_t>  w;            // (4 match states * 256 bytes) mixer-contexts * NIN
    APM apm1, apm2;
    uint8_t  hist[8] = {0};
    uint32_t h[NMODEL], idx[NMODEL];
    int      st[NIN];
    int      mixctx = 0, prMix = 2048;
    const Bytes* buf = nullptr; size_t pos = 0;
    uint32_t matchPtr = 0; int matchLen = 0; uint8_t mmByte = 0; bool mmAligned = false; int depth = 0;
    uint32_t node = 1;

    CM() : mhash(size_t(1) << MHBITS, 0), w(4 * 256 * NIN, (1 << 16) / NMODEL),
           apm1(256), apm2(65536) {
        for (int m = 0; m < NMODEL; ++m) tbl[m].assign(size_t(1) << TBITS, (uint32_t(1) << 31));
    }

    int matchState() const {
        if (!mmAligned) return 0;
        return matchLen < 16 ? 1 : matchLen < 32 ? 2 : 3;
    }

    void begin_byte(const Bytes* b, size_t p) {
        buf = b; pos = p; depth = 0;
        for (int m = 0; m < NMODEL; ++m) {
            uint32_t hh = 0x811c9dc5u;
            for (int i = 0; i < ORDERS[m]; ++i) hh = (hh ^ (hist[i] + 1)) * 0x6f4a7c13u;
            h[m] = hh + (uint32_t)(m * 0x9e3779b1u);
        }
        if (pos >= (size_t)MINMATCH) {
            uint32_t hh = 0x811c9dc5u;
            for (int i = 0; i < MINMATCH; ++i) hh = (hh ^ (*buf)[pos - 1 - i]) * 0x6f4a7c13u;
            hh &= MHMASK;
            if (matchLen == 0) {
                uint32_t cand = mhash[hh];
                if (cand) { matchPtr = cand; matchLen = 1; }
            }
            mhash[hh] = (uint32_t)pos;
        }
        mmAligned = (matchLen > 0 && matchPtr < pos);
        mmByte = mmAligned ? (*buf)[matchPtr] : 0;
    }

    int predict(uint32_t nd) {
        node = nd;
        mixctx = matchState() * 256 + hist[0];
        int64_t dot = 0;
        int32_t* ww = &w[mixctx * NIN];
        for (int m = 0; m < NMODEL; ++m) {
            uint32_t id = (h[m] + node * 2654435761u) & TMASK;
            idx[m] = id;
            st[m]  = stretch_tbl[counter_p12(tbl[m][id])];
            dot += (int64_t)ww[m] * st[m];
        }
        int stm = 0;
        if (mmAligned) {
            int expbit = (mmByte >> (7 - depth)) & 1;
            int s = 400 + matchLen * 64; if (s > 2047) s = 2047;
            stm = expbit ? s : -s;
        }
        st[NMODEL] = stm;
        st[NMODEL + 1] = 256;                          // bias input
        dot += (int64_t)ww[NMODEL] * stm + (int64_t)ww[NMODEL + 1] * 256;
        prMix = squash((int)(dot >> 16));
        prMix = std::min(4095, std::max(1, prMix));

        // two SSE stages: by the partial byte, and by the last byte + partial byte
        int p1 = apm1.refine(prMix, (int)node);
        int p2 = apm2.refine(prMix, (int)(((uint32_t)hist[0] << 8) | node));
        int pr = (prMix + p1 + 2 * p2 + 2) >> 2;
        return std::min(4095, std::max(1, pr));
    }

    void learn(int bit, int) {
        int err = (bit << 12) - prMix;
        int32_t* ww = &w[mixctx * NIN];
        for (int m = 0; m < NIN; ++m) {
            int nw = ww[m] + ((st[m] * err) >> 10);
            if (nw >  (1 << 22)) nw =  (1 << 22);
            if (nw < -(1 << 22)) nw = -(1 << 22);
            ww[m] = nw;
        }
        for (int m = 0; m < NMODEL; ++m) counter_update(tbl[m][idx[m]], bit);
        apm1.learn(bit);
        apm2.learn(bit);
        if (mmAligned && bit != ((mmByte >> (7 - depth)) & 1)) mmAligned = false;
        depth++;
    }

    void end_byte(uint8_t b) {
        if (matchLen > 0) {
            if (mmByte == b) { matchPtr++; if (matchLen < 65535) matchLen++; }
            else matchLen = 0;
        }
        for (int i = 7; i > 0; --i) hist[i] = hist[i - 1];
        hist[0] = b;
    }
};
}  // namespace v3

template <class Model>
static Bytes compress_with(const Bytes& data) {
    RangeEncoder enc;
    auto cm = std::make_unique<Model>();
    for (size_t i = 0; i < data.size(); ++i) {
        cm->begin_byte(&data, i);
        uint8_t byte = data[i];
        uint32_t node = 1;
        for (int shift = 7; shift >= 0; --shift) {
            int bit = (byte >> shift) & 1;
            int pr = cm->predict(node);
            enc.encode(bit, (uint32_t)pr);
            cm->learn(bit, pr);
            node = (node << 1) | bit;
        }
        cm->end_byte(byte);
    }
    return enc.finish();
}

template <class Model>
static Bytes decompress_with(const Bytes& comp, uint64_t out_size) {
    RangeDecoder dec(comp);
    auto cm = std::make_unique<Model>();
    Bytes out; out.reserve(out_size);
    for (uint64_t i = 0; i < out_size; ++i) {
        cm->begin_byte(&out, (size_t)i);
        uint32_t node = 1;
        for (int b = 0; b < 8; ++b) {
            int pr = cm->predict(node);
            int bit = dec.decode((uint32_t)pr);
            cm->learn(bit, pr);
            node = (node << 1) | bit;
        }
        uint8_t byte = uint8_t(node & 0xFF);
        out.push_back(byte);
        cm->end_byte(byte);
    }
    return out;
}

// ── CRC32 (IEEE, our own table — no zlib) ────────────────────────────────────
static uint32_t crc_table[256];
static void crc_init() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
}
static uint32_t crc32(const Bytes& d) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) c = crc_table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ── little-endian + file helpers ─────────────────────────────────────────────
static void put(Bytes& v, uint64_t x, int n) { for (int i = 0; i < n; i++) v.push_back(uint8_t(x >> (8 * i))); }
static bool get(const Bytes& v, size_t& p, int n, uint64_t& x) {
    if (p + n > v.size()) return false;
    x = 0; for (int i = 0; i < n; i++) x |= uint64_t(v[p++]) << (8 * i);
    return true;
}
static bool read_file(const fs::path& path, Bytes& out) {
    std::error_code ec;
    auto n = fs::file_size(path, ec);
    if (ec) return false;
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(path.c_str(), L"rb");
#else
    f = fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    out.resize((size_t)n);
    bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}
static bool write_file(const fs::path& path, const Bytes& d) {
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(path.c_str(), L"wb");
#else
    f = fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = d.empty() || fwrite(d.data(), 1, d.size(), f) == d.size();
    return fclose(f) == 0 && ok;
}

static const char* MAGIC_V2 = "CRUSH\x02";
static const char* MAGIC_V3 = "CRUSH\x03";
static const int MAGIC_LEN = 6;
enum { METHOD_RAW = 0, METHOD_CRUSH = 1 };

struct Entry { uint8_t method; std::string name; uint64_t osize; uint32_t crc; Bytes stored; };

// A folder on the command line adds everything under it, keeping the folder's name and the
// relative paths ("src/a.cpp", "src/sub/b.h"); a file is added under its own name.
static void gather(const std::string& arg, std::vector<std::pair<fs::path, std::string>>& out) {
    fs::path p = fs::u8path(arg);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        fs::path dir = fs::absolute(p, ec).lexically_normal();
        if (!dir.has_filename()) dir = dir.parent_path();          // "src\" -> "src"
        fs::path base = dir.parent_path();
        std::vector<fs::path> files;
        for (auto& e : fs::recursive_directory_iterator(dir, ec))
            if (e.is_regular_file(ec)) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (auto& f : files) out.push_back({ f, f.lexically_relative(base).generic_u8string() });
    } else {
        out.push_back({ p, p.filename().u8string() });
    }
}

static int do_create(const std::string& archive, const std::vector<std::string>& args) {
    std::vector<std::pair<fs::path, std::string>> files;
    for (auto& a : args) gather(a, files);
    Bytes buf;
    for (int i = 0; i < MAGIC_LEN; i++) buf.push_back(MAGIC_V3[i]);
    put(buf, files.size(), 4);
    uint64_t total_in = 0, total_out = 0;
    for (const auto& [path, name] : files) {
        Bytes data;
        if (!read_file(path, data)) { printf("  ! cannot read %s\n", path.u8string().c_str()); return 1; }
        Bytes comp = compress_with<v3::CM>(data);
        uint8_t method; const Bytes* stored;
        if (comp.size() < data.size()) { method = METHOD_CRUSH; stored = &comp; }
        else                           { method = METHOD_RAW;   stored = &data; }
        put(buf, method, 1);
        put(buf, name.size(), 2);
        for (char c : name) buf.push_back(uint8_t(c));
        put(buf, data.size(), 8);
        put(buf, crc32(data), 4);
        put(buf, stored->size(), 8);
        buf.insert(buf.end(), stored->begin(), stored->end());
        total_in += data.size(); total_out += stored->size();
        double pct = data.empty() ? 100.0 : 100.0 * stored->size() / data.size();
        printf("  + %-30s %10zu -> %10zu  (%5.1f%%)\n", name.c_str(), data.size(), stored->size(), pct);
    }
    if (!write_file(fs::u8path(archive), buf)) { printf("  ! cannot write %s\n", archive.c_str()); return 1; }
    double saved = total_in ? 100.0 * (1.0 - double(buf.size()) / total_in) : 0.0;
    printf("\n%s: %zu file(s), %llu -> %zu bytes (%.1f%% saved)\n",
           archive.c_str(), files.size(), (unsigned long long)total_in, buf.size(), saved);
    return 0;
}

static bool parse(const std::string& archive, std::vector<Entry>& entries, int& version) {
    Bytes buf;
    if (!read_file(fs::u8path(archive), buf)) { printf("cannot read %s\n", archive.c_str()); return false; }
    if (buf.size() < (size_t)(MAGIC_LEN + 4)) { printf("not a CRUSH archive\n"); return false; }
    if (std::equal(buf.begin(), buf.begin() + MAGIC_LEN, (const uint8_t*)MAGIC_V3)) version = 3;
    else if (std::equal(buf.begin(), buf.begin() + MAGIC_LEN, (const uint8_t*)MAGIC_V2)) version = 2;
    else if (buf[0] == 'C' && buf[5] == 1) { printf("this is a v1 archive: use the Python tool (crush.py)\n"); return false; }
    else { printf("not a CRUSH archive\n"); return false; }
    size_t p = MAGIC_LEN;
    uint64_t count, v;
    if (!get(buf, p, 4, count)) goto truncated;
    for (uint64_t i = 0; i < count; i++) {
        Entry e;
        if (!get(buf, p, 1, v)) goto truncated;
        e.method = (uint8_t)v;
        uint64_t nlen;
        if (!get(buf, p, 2, nlen) || p + nlen > buf.size()) goto truncated;
        e.name.assign((const char*)buf.data() + p, (size_t)nlen); p += (size_t)nlen;
        if (!get(buf, p, 8, e.osize) || !get(buf, p, 4, v)) goto truncated;
        e.crc = (uint32_t)v;
        uint64_t ssize;
        if (!get(buf, p, 8, ssize) || ssize > buf.size() - p) goto truncated;
        e.stored.assign(buf.begin() + p, buf.begin() + p + (size_t)ssize); p += (size_t)ssize;
        if (e.method == METHOD_RAW && e.osize != ssize) goto truncated;
        entries.push_back(std::move(e));
    }
    return true;
truncated:
    printf("archive is damaged or truncated\n");
    return false;
}

static Bytes restore(const Entry& e, int version) {
    if (e.method == METHOD_RAW) return e.stored;
    return version == 2 ? decompress_with<v2::CM>(e.stored, e.osize) : decompress_with<v3::CM>(e.stored, e.osize);
}

// Never write outside the destination: no absolute paths, no "..", no drive letters.
static bool safe_relative(const std::string& name, fs::path& out) {
    fs::path p = fs::u8path(name);
    if (name.empty() || p.has_root_name() || p.has_root_directory()) return false;
    for (auto& part : p) if (part == "..") return false;
    out = p;
    return true;
}

static int do_extract(const std::string& archive, const std::string& dest) {
    std::vector<Entry> es; int version = 0;
    if (!parse(archive, es, version)) return 1;
    int bad = 0;
    for (auto& e : es) {
        fs::path rel;
        if (!safe_relative(e.name, rel)) { printf("  SKIP %s (unsafe path)\n", e.name.c_str()); bad++; continue; }
        fs::path target = fs::u8path(dest) / rel;
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        Bytes data = restore(e, version);
        bool ok = crc32(data) == e.crc && data.size() == e.osize;
        if (!write_file(target, data)) { printf("  ! cannot write %s\n", target.u8string().c_str()); bad++; continue; }
        printf("  %s  %s  (%llu bytes)\n", ok ? "OK " : "BAD", e.name.c_str(), (unsigned long long)e.osize);
        if (!ok) bad++;
    }
    return bad == 0 ? 0 : 2;
}

static int do_list(const std::string& archive) {
    std::vector<Entry> es; int version = 0;
    if (!parse(archive, es, version)) return 1;
    printf("CRUSH v%d archive\n%-30s %12s %12s %7s  method\n", version, "name", "size", "stored", "ratio");
    for (auto& e : es) {
        double pct = e.osize ? 100.0 * e.stored.size() / e.osize : 100.0;
        printf("%-30s %12llu %12zu %6.1f%%  %s\n", e.name.c_str(),
               (unsigned long long)e.osize, e.stored.size(), pct,
               e.method == METHOD_CRUSH ? "crush" : "raw");
    }
    return 0;
}

static int do_test(const std::string& archive) {
    std::vector<Entry> es; int version = 0;
    if (!parse(archive, es, version)) return 1;
    int bad = 0;
    for (auto& e : es) {
        Bytes data = restore(e, version);
        bool ok = crc32(data) == e.crc && data.size() == e.osize;
        printf("  %s  %s\n", ok ? "OK " : "BAD", e.name.c_str());
        if (!ok) bad++;
    }
    printf(bad == 0 ? "All files OK.\n" : "%d file(s) failed.\n", bad);
    return bad == 0 ? 0 : 2;
}

static int run(int argc, const std::vector<std::string>& argv) {
    crc_init();
    mix_init();
    v3::counters_init();
    if (argc < 3) {
        printf("CRUSH — from-scratch compressor\n"
               "  crush a out.crush file_or_folder ...   create\n"
               "  crush x out.crush [dest]              extract\n"
               "  crush e out.crush                     extract to a folder named after it\n"
               "  crush l out.crush                     list\n"
               "  crush t out.crush                     test\n");
        return 1;
    }
    std::string cmd = argv[1], archive = argv[2];
    std::vector<std::string> rest(argv.begin() + 3, argv.end());
    if (cmd == "a") { if (rest.empty()) { printf("nothing to add\n"); return 1; } return do_create(archive, rest); }
    if (cmd == "x") return do_extract(archive, rest.empty() ? "." : rest[0]);
    if (cmd == "e") {                                   // "backup.crush" -> a "backup" folder next to it
        fs::path a = fs::u8path(archive);
        return do_extract(archive, (a.parent_path() / a.stem()).u8string());
    }
    if (cmd == "l") return do_list(archive);
    if (cmd == "t") return do_test(archive);
    printf("unknown command '%s'\n", cmd.c_str());
    return 1;
}

// Arguments as UTF-8 everywhere: on Windows take the wide command line (the ANSI argv would
// mangle names like "Farica☆Kimora.txt") and print UTF-8 to the console.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
int wmain(int argc, wchar_t** wargv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(fs::path(wargv[i]).u8string());
    return run(argc, args);
}
#else
int main(int argc, char** argv) {
    return run(argc, std::vector<std::string>(argv, argv + argc));
}
#endif
