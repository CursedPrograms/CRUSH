// CRUSH — a from-scratch compressor/archiver in C++.
//
// Same math and the SAME .crush format as the Python version (crush.py), so an
// archive made by either tool is readable by the other. The compression is ours:
// a binary range coder (arithmetic coding) over an adaptive order-1 context model.
// No zlib, no libraries — just <cstdint>, <vector>, <string>, file I/O.
//
// Build:   g++ -O2 -o crush crush.cpp
// Use:     crush a out.crush file1 file2 ...   create / add
//          crush x out.crush [dest_dir]        extract
//          crush l out.crush                   list + ratios
//          crush t out.crush                   test integrity (CRC32)

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>

using std::uint8_t; using std::uint16_t; using std::uint32_t; using std::uint64_t;
using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;

// ── compression math (mirrors crush/core.py exactly) ────────────────────────
static const int      PBITS   = 12;
static const uint32_t PSCALE  = 1u << PBITS;   // 4096
static const uint32_t PHALF   = PSCALE >> 1;   // 2048
static const int      ADAPT   = 5;
static const uint32_t CTX_SIZE = 256 * 256;

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

// ── context mixing (the v2 improvement) ──────────────────────────────────────
// Several context orders each predict the next bit; a small adaptive mixer blends
// their predictions in the logistic domain (stretch -> weighted sum -> squash).
// Weights learn online, per mixer-context, so good predictors earn more say.
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
    // match model
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
        // Match model: hash the last MINMATCH bytes; if we're not already extending
        // a match, see if that context was seen before and point there.
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
        int stm = 0;                           // match model's vote
        if (mmAligned) {
            int expbit = (mmByte >> (7 - depth)) & 1;
            int s = 400 + matchLen * 64; if (s > 2047) s = 2047;   // longer match = louder
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
        if (matchLen > 0) {                    // extend the match if it predicted the whole byte
            if (mmByte == b) { matchPtr++; if (matchLen < 65535) matchLen++; }
            else matchLen = 0;
        }
        for (int i = 7; i > 0; --i) hist[i] = hist[i - 1];
        hist[0] = b;
        mixctx = b;
    }
};

static Bytes compress(const Bytes& data) {
    RangeEncoder enc;
    CM cm;
    for (size_t i = 0; i < data.size(); ++i) {
        cm.begin_byte(&data, i);
        uint8_t byte = data[i];
        uint32_t node = 1;
        for (int shift = 7; shift >= 0; --shift) {
            int bit = (byte >> shift) & 1;
            int pr = cm.predict(node);
            enc.encode(bit, (uint32_t)pr);
            cm.learn(bit, pr);
            node = (node << 1) | bit;
        }
        cm.end_byte(byte);
    }
    return enc.finish();
}

static Bytes decompress(const Bytes& comp, uint64_t out_size) {
    RangeDecoder dec(comp);
    CM cm;
    Bytes out; out.reserve(out_size);
    for (uint64_t i = 0; i < out_size; ++i) {
        cm.begin_byte(&out, (size_t)i);
        uint32_t node = 1;
        for (int b = 0; b < 8; ++b) {
            int pr = cm.predict(node);
            int bit = dec.decode((uint32_t)pr);
            cm.learn(bit, pr);
            node = (node << 1) | bit;
        }
        uint8_t byte = uint8_t(node & 0xFF);
        out.push_back(byte);
        cm.end_byte(byte);
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
static uint64_t get(const Bytes& v, size_t& p, int n) { uint64_t x = 0; for (int i = 0; i < n; i++) x |= uint64_t(v[p++]) << (8 * i); return x; }
static bool read_file(const std::string& path, Bytes& out) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    if (n > 0 && fread(out.data(), 1, n, f) != (size_t)n) { fclose(f); return false; }
    fclose(f); return true;
}
static bool write_file(const std::string& path, const Bytes& d) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    if (!d.empty()) fwrite(d.data(), 1, d.size(), f);
    fclose(f); return true;
}

static const char* MAGIC = "CRUSH\x02";   // v2 = context-mixing model (v1 = Python order-1)
static const int MAGIC_LEN = 6;
enum { METHOD_RAW = 0, METHOD_CRUSH = 1 };

struct Entry { uint8_t method; std::string name; uint64_t osize; uint32_t crc; Bytes stored; };

static int do_create(const std::string& archive, const std::vector<std::string>& files) {
    Bytes buf;
    for (int i = 0; i < MAGIC_LEN; i++) buf.push_back(MAGIC[i]);
    put(buf, files.size(), 4);
    uint64_t total_in = 0, total_out = 0;
    for (const auto& path : files) {
        Bytes data;
        if (!read_file(path, data)) { printf("  ! cannot read %s\n", path.c_str()); return 1; }
        Bytes comp = compress(data);
        uint8_t method; const Bytes* stored;
        if (comp.size() < data.size()) { method = METHOD_CRUSH; stored = &comp; }
        else                           { method = METHOD_RAW;   stored = &data; }
        std::string name = fs::path(path).filename().string();
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
    if (!write_file(archive, buf)) { printf("  ! cannot write %s\n", archive.c_str()); return 1; }
    double saved = total_in ? 100.0 * (1.0 - double(buf.size()) / total_in) : 0.0;
    printf("\n%s: %zu file(s), %llu -> %zu bytes (%.1f%% saved)\n",
           archive.c_str(), files.size(), (unsigned long long)total_in, buf.size(), saved);
    return 0;
}

static bool parse(const std::string& archive, std::vector<Entry>& entries) {
    Bytes buf;
    if (!read_file(archive, buf)) { printf("cannot read %s\n", archive.c_str()); return false; }
    if (buf.size() < (size_t)(MAGIC_LEN + 4)) { printf("not a CRUSH archive\n"); return false; }
    for (int i = 0; i < MAGIC_LEN; i++) if (buf[i] != (uint8_t)MAGIC[i]) { printf("not a CRUSH archive\n"); return false; }
    size_t p = MAGIC_LEN;
    uint64_t count = get(buf, p, 4);
    for (uint64_t i = 0; i < count; i++) {
        Entry e;
        e.method = (uint8_t)get(buf, p, 1);
        uint64_t nlen = get(buf, p, 2);
        e.name.assign((const char*)&buf[p], nlen); p += nlen;
        e.osize = get(buf, p, 8);
        e.crc = (uint32_t)get(buf, p, 4);
        uint64_t ssize = get(buf, p, 8);
        e.stored.assign(buf.begin() + p, buf.begin() + p + ssize); p += ssize;
        entries.push_back(std::move(e));
    }
    return true;
}

static Bytes restore(const Entry& e) {
    return e.method == METHOD_RAW ? e.stored : decompress(e.stored, e.osize);
}

static int do_extract(const std::string& archive, const std::string& dest) {
    std::vector<Entry> es; if (!parse(archive, es)) return 1;
    fs::create_directories(dest);
    for (auto& e : es) {
        Bytes data = restore(e);
        bool ok = crc32(data) == e.crc && data.size() == e.osize;
        write_file((fs::path(dest) / fs::path(e.name).filename()).string(), data);
        printf("  %s  %s  (%llu bytes)\n", ok ? "OK " : "BAD", e.name.c_str(), (unsigned long long)e.osize);
    }
    return 0;
}

static int do_list(const std::string& archive) {
    std::vector<Entry> es; if (!parse(archive, es)) return 1;
    printf("%-30s %12s %12s %7s  method\n", "name", "size", "stored", "ratio");
    for (auto& e : es) {
        double pct = e.osize ? 100.0 * e.stored.size() / e.osize : 100.0;
        printf("%-30s %12llu %12zu %6.1f%%  %s\n", e.name.c_str(),
               (unsigned long long)e.osize, e.stored.size(), pct,
               e.method == METHOD_CRUSH ? "crush" : "raw");
    }
    return 0;
}

static int do_test(const std::string& archive) {
    std::vector<Entry> es; if (!parse(archive, es)) return 1;
    int bad = 0;
    for (auto& e : es) {
        Bytes data = restore(e);
        bool ok = crc32(data) == e.crc && data.size() == e.osize;
        printf("  %s  %s\n", ok ? "OK " : "BAD", e.name.c_str());
        if (!ok) bad++;
    }
    printf(bad == 0 ? "All files OK.\n" : "%d file(s) failed.\n", bad);
    return bad == 0 ? 0 : 2;
}

int main(int argc, char** argv) {
    crc_init();
    mix_init();
    if (argc < 3) {
        printf("CRUSH — from-scratch compressor\n"
               "  crush a out.crush file1 file2 ...   create\n"
               "  crush x out.crush [dest]            extract\n"
               "  crush l out.crush                   list\n"
               "  crush t out.crush                   test\n");
        return 1;
    }
    std::string cmd = argv[1], archive = argv[2];
    std::vector<std::string> rest(argv + 3, argv + argc);
    if (cmd == "a") { if (rest.empty()) { printf("nothing to add\n"); return 1; } return do_create(archive, rest); }
    if (cmd == "x") return do_extract(archive, rest.empty() ? "." : rest[0]);
    if (cmd == "l") return do_list(archive);
    if (cmd == "t") return do_test(archive);
    printf("unknown command '%s'\n", cmd.c_str());
    return 1;
}
