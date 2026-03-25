// =============================================================================
// tisa_test_x64.cpp — TISA VM x64 test runner
//
// Reads the same binary files as test_TISA_VM.ino (ESP32):
//   tisa_test_suite.bin        — test cases (manifest + text + ref_ids)
//   models/model_map.txt       — model_id:hash mapping
//   models/<hash>/vocab.b      — vocabulary
//   models/<hash>/vocab_idx.b  — id→offset reverse index (for decode)
//   models/<hash>/merges.b     — BPE merges (optional)
//
//   • Entire vocab/merges loaded into unordered_map at startup (O(1) lookup)
//   • No SD card I/O during inference; no mutex; no block cache
//   • string_view for fragment iteration in partition_rules
//   • pgm_read_dword() → plain dereference (no PROGMEM overhead)
//   • -O3 -march=native lets the compiler vectorise inner loops
//
// Copyright (c) 2026 Dmitry Feklin (FeklinDN@gmail.com). Apache License 2.0.
// =============================================================================

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

// ── Macros for UCD tables compatibility ──────────────────────────────────────
#define PROGMEM
#define pgm_read_dword(addr) (*reinterpret_cast<const uint32_t*>(addr))

// ── Standard headers ─────────────────────────────────────────────────────────
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ── UCD tables (same header as ESP32, works with pgm_read_dword stub) ────────
// Expects TISA_UCD_TABLES.h in the include path (same file as on ESP32).
#include "TISA_UCD_TABLES.h"

// =============================================================================
// UTF-8 utilities (identical to TISA_VM.cpp)
// =============================================================================
static inline size_t get_utf8_char_len(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0;
}

static inline uint32_t utf8_to_codepoint(const unsigned char* s, size_t len) {
    switch (len) {
        case 1: return s[0];
        case 2: return ((s[0] & 0x1F) << 6)  | (s[1] & 0x3F);
        case 3: return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6)  | (s[2] & 0x3F);
        case 4: return ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        default: return 0;
    }
}

std::string codepoint_to_utf8(uint32_t cp) {
    std::string r;
    if      (cp < 0x80)    { r += (char)cp; }
    else if (cp < 0x800)   { r += (char)(0xC0|(cp>>6)); r += (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000) { r += (char)(0xE0|(cp>>12)); r += (char)(0x80|((cp>>6)&0x3F)); r += (char)(0x80|(cp&0x3F)); }
    else                   { r += (char)(0xF0|(cp>>18)); r += (char)(0x80|((cp>>12)&0x3F)); r += (char)(0x80|((cp>>6)&0x3F)); r += (char)(0x80|(cp&0x3F)); }
    return r;
}

// =============================================================================
// Unicode category helpers (same as TISA_VM.cpp)
// =============================================================================
static bool is_in_category_ranges(uint32_t cp, const CategoryRange* ranges, size_t count) {
    int lo = 0, hi = (int)count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t s = pgm_read_dword(&ranges[mid].start);
        uint32_t e = pgm_read_dword(&ranges[mid].end);
        if (cp < s) hi = mid - 1; else if (cp > e) lo = mid + 1; else return true;
    }
    return false;
}

static bool is_category(uint32_t cp, const std::string& cat) {
    if (cat == "P")  return is_in_category_ranges(cp, CAT_P_RANGES,  sizeof(CAT_P_RANGES) /sizeof(CategoryRange));
    if (cat == "Z")  return is_in_category_ranges(cp, CAT_Z_RANGES,  sizeof(CAT_Z_RANGES) /sizeof(CategoryRange));
    if (cat == "Mn") return is_in_category_ranges(cp, CAT_MN_RANGES, sizeof(CAT_MN_RANGES)/sizeof(CategoryRange));
    if (cat == "Cc") return is_in_category_ranges(cp, CAT_CC_RANGES, sizeof(CAT_CC_RANGES)/sizeof(CategoryRange));
    if (cat == "Cf") return is_in_category_ranges(cp, CAT_CF_RANGES, sizeof(CAT_CF_RANGES)/sizeof(CategoryRange));
    return false;
}

static bool is_whitespace(uint32_t cp) {
    return (cp >= 0x0009 && cp <= 0x000D) || cp == 0x0020 || cp == 0x00A0 || is_category(cp, "Z");
}

enum CharType { LETTER, NUMBER, WHITESPACE, OTHER };
static CharType get_char_type(uint32_t cp) {
    if (is_whitespace(cp)) return WHITESPACE;
    if ((cp>='a'&&cp<='z')||(cp>='A'&&cp<='Z')||(cp>=0x0400&&cp<=0x04FF)||(cp>=0x00C0&&cp<=0x02AF)) return LETTER;
    if (cp >= '0' && cp <= '9') return NUMBER;
    return OTHER;
}

static bool is_cjk(uint32_t cp) {
    return (cp>=0x4E00&&cp<=0x9FFF)||(cp>=0x3040&&cp<=0x309F)||(cp>=0x30A0&&cp<=0x30FF)||(cp>=0xAC00&&cp<=0xD7AF);
}

static size_t find_gpt_next_token(const std::string& text, size_t start, size_t& out_end) {
    if (start >= text.length()) return std::string::npos;
    if (text[start] == '\'') {
        const char* contractions[] = {"'s","'t","'re","'ve","'m","'ll","'d"};
        for (const char* c : contractions) {
            size_t l = strlen(c);
            if (text.compare(start, l, c) == 0) { out_end = start + l; return start; }
        }
    }
    size_t cur = start;
    if (text[cur] == ' ') ++cur;
    if (cur < text.length()) {
        size_t cl = get_utf8_char_len(text[cur]);
        if (cl > 0) {
            uint32_t cp = utf8_to_codepoint((const unsigned char*)&text[cur], cl);
            CharType type = get_char_type(cp);
            if (type != WHITESPACE) {
                size_t ge = cur;
                while (ge < text.length()) {
                    cl = get_utf8_char_len(text[ge]); if (!cl) break;
                    CharType t = get_char_type(utf8_to_codepoint((const unsigned char*)&text[ge], cl));
                    if (type == LETTER && t != LETTER) break;
                    if (type == NUMBER && t != NUMBER) break;
                    if (type == OTHER  && (t==WHITESPACE||t==LETTER||t==NUMBER)) break;
                    ge += cl;
                }
                out_end = ge; return start;
            }
        }
    }
    size_t cl = get_utf8_char_len(text[start]);
    if (cl > 0 && is_whitespace(utf8_to_codepoint((const unsigned char*)&text[start], cl))) {
        size_t e = start;
        while (e < text.length()) {
            cl = get_utf8_char_len(text[e]); if (!cl) break;
            if (!is_whitespace(utf8_to_codepoint((const unsigned char*)&text[e], cl))) break;
            e += cl;
        }
        out_end = e; return start;
    }
    out_end = start + 1; return start;
}

// =============================================================================
// x64 Resource structures
// =============================================================================

struct VocabEntry { int32_t id = -1; float score = 0.f; };

using VocabMap   = std::unordered_map<std::string, VocabEntry>;
using MergesMap  = std::unordered_map<std::string, int32_t>;
using VocabIndex = std::vector<uint32_t>;
using VocabData  = std::vector<uint8_t>;
using ByteMapArr = std::array<std::string, 256>;

struct X64Resources {
    VocabMap   vocab;
    MergesMap  merges;
    VocabIndex vocab_idx;
    VocabData  vocab_data;
    ByteMapArr byte_map;
    // O(1) Fast map for byte decoding: packed utf8 char bytes -> byte index
    std::unordered_map<uint32_t, uint8_t> fast_rev_map;
    bool       merges_loaded = false;
};

// ── File helpers ──────────────────────────────────────────────────────────────
static std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read((char*)buf.data(), sz);
    return buf;
}

static std::string read_str16(const uint8_t* data, size_t data_size, size_t& pos) {
    if (pos + 2 > data_size) return {};
    uint16_t len; memcpy(&len, data + pos, 2); pos += 2;
    if (pos + len > data_size) return {};
    std::string s((const char*)data + pos, len); pos += len;
    return s;
}

static ByteMapArr build_byte_map() {
    ByteMapArr bm;
    bool used[256] = {};
    for (int i = 33; i <= 126; ++i) used[i] = true;
    for (int i = 161; i <= 172; ++i) used[i] = true;
    for (int i = 174; i <= 255; ++i) used[i] = true;
    for (int i = 0; i < 256; ++i)
        if (used[i]) bm[i] = codepoint_to_utf8(i);
    uint32_t extra = 0;
    for (int i = 0; i < 256; ++i)
        if (!used[i]) bm[i] = codepoint_to_utf8(256 + extra++);
    return bm;
}

static bool load_vocab(const std::string& path, X64Resources& res) {
    auto raw = read_file_bytes(path);
    if (raw.size() < 4) return false;

    uint32_t count; memcpy(&count, raw.data(), 4);
    size_t offset_table_end = 4 + (size_t)count * 4;
    if (raw.size() < offset_table_end) return false;

    const uint32_t* offsets = (const uint32_t*)(raw.data() + 4);
    const uint8_t*  ds      = raw.data() + offset_table_end;
    size_t          ds_size = raw.size() - offset_table_end;

    res.vocab_data.assign(ds, ds + ds_size);
    res.vocab.reserve(count * 2);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t rel = offsets[i];
        if (rel + 2 > ds_size) continue;
        size_t pos = rel;
        std::string key = read_str16(ds, ds_size, pos);
        if (key.empty() && pos > rel + 2) continue;
        if (pos + 8 > ds_size) continue;
        int32_t id; float score;
        memcpy(&id,    ds + pos, 4); pos += 4;
        memcpy(&score, ds + pos, 4);
        res.vocab[key] = {id, score};
    }
    return true;
}

static bool load_vocab_idx(const std::string& path, X64Resources& res) {
    auto raw = read_file_bytes(path);
    if (raw.size() < 4) return false;
    uint32_t count; memcpy(&count, raw.data(), 4);
    if (raw.size() < 4 + (size_t)count * 4) return false;
    res.vocab_idx.resize(count);
    memcpy(res.vocab_idx.data(), raw.data() + 4, count * 4);
    return true;
}

static bool load_merges(const std::string& path, X64Resources& res) {
    auto raw = read_file_bytes(path);
    if (raw.size() < 4) return false;
    uint32_t count; memcpy(&count, raw.data(), 4);
    size_t ot_end = 4 + (size_t)count * 4;
    if (raw.size() < ot_end) return false;
    const uint32_t* offsets = (const uint32_t*)(raw.data() + 4);
    const uint8_t*  ds      = raw.data() + ot_end;
    size_t          ds_size = raw.size() - ot_end;
    res.merges.reserve(count * 2);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t rel = offsets[i];
        size_t pos = rel;
        std::string t1 = read_str16(ds, ds_size, pos);
        std::string t2 = read_str16(ds, ds_size, pos);
        if (pos + 4 > ds_size) continue;
        int32_t rank; memcpy(&rank, ds + pos, 4);
        std::string key; key.reserve(t1.size() + 1 + t2.size());
        key += t1; key += '\0'; key += t2;
        res.merges[std::move(key)] = rank;
    }
    res.merges_loaded = true;
    return true;
}

static std::string_view token_view_by_id(const X64Resources& res, int32_t id) {
    if (id < 0 || (size_t)id >= res.vocab_idx.size()) return {};
    uint32_t off = res.vocab_idx[id];
    if (off == 0xFFFFFFFF || off + 2 > res.vocab_data.size()) return {};
    
    uint16_t len;
    memcpy(&len, res.vocab_data.data() + off, 2);
    if (off + 2 + len > res.vocab_data.size()) return {};
    
    return std::string_view(reinterpret_cast<const char*>(res.vocab_data.data() + off + 2), len);
}

// =============================================================================
// TISA encode VM
// =============================================================================
struct Fragment { std::string text; bool is_protected = false; };
struct TISA_State { std::string text; std::vector<Fragment> fragments; std::vector<int32_t> ids; };

static std::string unescape_pattern(const std::string& p) {
    std::string r; r.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\\' && i + 1 < p.size()) { r += p[++i]; }
        else                                    { r += p[i]; }
    }
    return r;
}

struct Rule {
    std::string pattern;
    bool        is_protected = false;
    std::string behavior;
    std::string trim_preceding_space;
};

static void op_partition_rules(TISA_State& state, const uint8_t* p, size_t /*len*/);
static void op_bpe_encode     (TISA_State& state, const X64Resources& res);
static void op_wordpiece_encode(TISA_State& state, const X64Resources& res, const std::string& marker);
static void op_unigram_encode  (TISA_State& state, const X64Resources& res);

static void dispatch_opcode(uint8_t op, const uint8_t* p, size_t len,
                             TISA_State& s, const X64Resources& res)
{
    switch (op) {
    case 0x01: {
        const size_t exc_count = sizeof(LOWERCASE_EXCEPTIONS)/sizeof(UnicodeException);
        const size_t rng_count = sizeof(LOWERCASE_RANGES)/sizeof(UnicodeRange);
        std::string result;
        for (size_t i = 0; i < s.text.length(); ) {
            size_t cl = get_utf8_char_len(s.text[i]); if (!cl){ ++i; continue; }
            uint32_t cp = utf8_to_codepoint((const unsigned char*)&s.text[i], cl);
            uint32_t lo = cp;
            { int a=0,b=(int)exc_count-1;
              while(a<=b){int m=a+(b-a)/2; uint32_t f=pgm_read_dword(&LOWERCASE_EXCEPTIONS[m].from);
                if(cp==f){lo=pgm_read_dword(&LOWERCASE_EXCEPTIONS[m].to);break;}
                else if(cp<f)b=m-1;else a=m+1;}}
            if (lo==cp){int a=0,b=(int)rng_count-1;
              while(a<=b){int m=a+(b-a)/2;uint32_t ss=pgm_read_dword(&LOWERCASE_RANGES[m].start),ee=pgm_read_dword(&LOWERCASE_RANGES[m].end);
                if(cp<ss)b=m-1;else if(cp>ee)a=m+1;else{lo=(uint32_t)((int32_t)cp+(int32_t)pgm_read_dword(&LOWERCASE_RANGES[m].delta));break;}}}
            result += codepoint_to_utf8(lo); i += cl;
        }
        s.text = std::move(result);
        break;
    }
    case 0x02: {
        std::string form((char*)&p[1], p[0]);
        if (form == "NFD") {
            const size_t dc = sizeof(DECOMP_TABLE)/sizeof(Decomp);
            std::string decomposed;
            for (size_t i = 0; i < s.text.length(); ) {
                size_t cl = get_utf8_char_len(s.text[i]); if(!cl){++i;continue;}
                uint32_t cp = utf8_to_codepoint((const unsigned char*)&s.text[i], cl);
                int a=0,b=(int)dc-1; bool found=false;
                while(a<=b){int m=a+(b-a)/2; uint32_t f=pgm_read_dword(&DECOMP_TABLE[m].from);
                  if(cp==f){decomposed+=codepoint_to_utf8(pgm_read_dword(&DECOMP_TABLE[m].to1));
                    uint32_t t2=pgm_read_dword(&DECOMP_TABLE[m].to2); if(t2)decomposed+=codepoint_to_utf8(t2);
                    found=true;break;}else if(cp<f)b=m-1;else a=m+1;}
                if(!found) decomposed += s.text.substr(i, cl);
                i += cl;
            }
            s.text = std::move(decomposed);
        }
        break;
    }
    case 0x03: {
        // Fix: Use memcpy to avoid unaligned pointer UB
        uint16_t pat_len; memcpy(&pat_len, &p[0], 2);
        std::string pat((char*)&p[2], pat_len);
        uint16_t val_len; memcpy(&val_len, &p[2 + pat_len], 2);
        std::string val((char*)&p[4 + pat_len], val_len);
        
        for (size_t pos=0; (pos=s.text.find(pat,pos))!=std::string::npos; pos+=val.size())
            s.text.replace(pos, pat.size(), val);
        break;
    }
    case 0x04: {
        uint8_t nc = p[0]; const uint8_t* ptr = &p[1];
        std::vector<std::string> cats; cats.reserve(nc);
        for (int i=0;i<nc;++i){ uint8_t l=*ptr++; cats.push_back({(char*)ptr,l}); ptr+=l; }
        std::string out;
        for (size_t i=0;i<s.text.length();) {
            size_t cl=get_utf8_char_len(s.text[i]); if(!cl){++i;continue;}
            uint32_t cp=utf8_to_codepoint((const unsigned char*)&s.text[i],cl);
            bool filt=false; for(auto&c:cats) if(is_category(cp,c)){filt=true;break;}
            if(!filt) out+=s.text.substr(i,cl);
            i+=cl;
        }
        s.text=std::move(out);
        break;
    }
    case 0x07: {
        // Fix: Use memcpy to avoid unaligned pointer UB
        uint16_t val_len; memcpy(&val_len, &p[0], 2);
        std::string val((char*)&p[2], val_len);
        if (s.text.rfind(val,0)!=0) s.text.insert(0, val);
        break;
    }
    case 0x10: op_partition_rules(s, p, len); break;
    case 0x15: {
        if (s.fragments.empty() && !s.text.empty())
            s.fragments.push_back({s.text, false});
        for (auto& f : s.fragments) {
            if (f.is_protected) continue;
            std::string enc; enc.reserve(f.text.size() * 2);
            for (unsigned char b : f.text)
                enc += res.byte_map[b];
            f.text = std::move(enc);
        }
        break;
    }
    case 0x20: op_bpe_encode(s, res); break;
    case 0x21: {
        std::string marker((char*)&p[1], p[0]);
        op_wordpiece_encode(s, res, marker);
        break;
    }
    case 0x22: op_unigram_encode(s, res); break;
    case 0x30: {
        std::vector<int32_t> out;
        uint8_t n = p[0]; const uint8_t* ptr = &p[1];
        for (int i=0;i<n;++i) {
            if (ptr[0]) { // FIXED
                ptr++;
                if (ptr[0]) { // int
                    ptr++; int32_t id; memcpy(&id,ptr,4); ptr+=4; out.push_back(id);
                } else { // string name
                    ptr++; std::string tok((char*)&ptr[1],ptr[0]); ptr+=1+tok.size();
                    auto it=res.vocab.find(tok);
                    out.push_back(it!=res.vocab.end() ? it->second.id : 2);
                }
            } else { // SLOT
                ptr++; out.insert(out.end(),s.ids.begin(),s.ids.end());
            }
        }
        s.ids = std::move(out);
        break;
    }
    }
}

static void op_partition_rules(TISA_State& state, const uint8_t* p, size_t /*len*/) {
    state.fragments.clear();
    const std::string& text = state.text;
    uint16_t num_rules; memcpy(&num_rules, p, 2);
    const uint8_t* ptr = p + 2;
    if (num_rules == 0) {
        if (!text.empty()) state.fragments.push_back({text, false});
        return;
    }
    std::vector<Rule> rules; rules.reserve(num_rules);
    for (uint16_t i = 0; i < num_rules; ++i) {
        Rule rule;
        uint8_t flags = *ptr++;
        rule.is_protected = (flags & 1);
        uint16_t pl; memcpy(&pl, ptr, 2); ptr += 2;
        rule.pattern.assign((const char*)ptr, pl); ptr += pl;
        if (flags & 2) { uint8_t b=*ptr++; rule.behavior=(b==1)?"REMOVE":(b==2?"ISOLATE":""); }
        if (flags & 4) { uint8_t tl=*ptr++; rule.trim_preceding_space.assign((const char*)ptr,tl); ptr+=tl; }
        rules.push_back(std::move(rule));
    }

    size_t last = 0;
    while (last < text.length()) {
        size_t bs = std::string::npos, be = 0; int bi = -1;
        for (size_t i = 0; i < rules.size(); ++i) {
            const Rule& rule = rules[i];
            size_t ms = std::string::npos, me = 0;
            if (rule.pattern.rfind("(?: ?)", 0) == 0) {
                std::string lit = unescape_pattern(rule.pattern.substr(6));
                size_t p1=text.find(lit,last), p2=text.find(" "+lit,last);
                if (p1!=std::string::npos&&(p2==std::string::npos||p1<=p2)) { ms=p1; me=p1+lit.size(); }
                else if (p2!=std::string::npos) { ms=p2; me=p2+1+lit.size(); }
            } else if (rule.pattern == "\\p{P}") {
                for (size_t k=last;k<text.length();) {
                    size_t cl=get_utf8_char_len(text[k]); if(!cl){++k;continue;}
                    uint32_t cp=utf8_to_codepoint((const unsigned char*)&text[k],cl);
                    if ((cp=='.'||cp==','||cp=='!'||cp=='?'||cp==':'||cp==';'||cp=='"'||cp=='\''||
                         cp=='('||cp==')'||cp=='['||cp==']'||cp=='{'||cp=='}')||is_category(cp,"P"))
                        { ms=k; me=k+cl; break; }
                    k+=cl;
                }
            } else if (rule.pattern == "\\s+") {
                for (size_t k=last;k<text.length();) {
                    size_t cl=get_utf8_char_len(text[k]); if(!cl){++k;continue;}
                    if (is_whitespace(utf8_to_codepoint((const unsigned char*)&text[k],cl))) {
                        ms=k; size_t e=k+cl;
                        while(e<text.length()){size_t nl=get_utf8_char_len(text[e]);if(!nl||!is_whitespace(utf8_to_codepoint((const unsigned char*)&text[e],nl)))break;e+=nl;}
                        me=e; break;
                    }
                    k+=cl;
                }
            } else if (rule.pattern.find("\xe4\xb8\x80")!=std::string::npos ||
                       rule.pattern.find("\\u4E00")!=std::string::npos) {
                for (size_t k=last;k<text.length();) {
                    size_t cl=get_utf8_char_len(text[k]); if(!cl){++k;continue;}
                    if (is_cjk(utf8_to_codepoint((const unsigned char*)&text[k],cl))) { ms=k; me=k+cl; break; }
                    k+=cl;
                }
            } else if (rule.pattern.find("'s|'t|'re") != std::string::npos) {
                ms = find_gpt_next_token(text, last, me);
            } else {
                char lit[256]; size_t ll = 0;
                const char* pp = rule.pattern.data(); size_t plen = rule.pattern.size();
                for (size_t j=0;j<plen&&ll<254;++j)
                    lit[ll++] = (pp[j]=='\\'&&j+1<plen) ? pp[++j] : pp[j];
                if (ll > 0) {
                    const char* hay = text.data(); size_t hn = text.length();
                    for (size_t k=last; k+ll<=hn; ++k)
                        if (memcmp(hay+k, lit, ll)==0) { ms=k; me=k+ll; break; }
                }
            }
            if (ms != std::string::npos) {
                if (ms < bs || (ms==bs && (me-ms)>(be-bs))) { bs=ms; be=me; bi=(int)i; }
            }
        }
        if (bi == -1) {
            if (last < text.length()) state.fragments.push_back({text.substr(last), false});
            break;
        }
        const Rule& br = rules[bi];
        if (bs > last) {
            std::string pre = text.substr(last, bs - last);
            if (!br.trim_preceding_space.empty() &&
                pre.size() >= br.trim_preceding_space.size() &&
                pre.compare(pre.size()-br.trim_preceding_space.size(),
                            br.trim_preceding_space.size(),
                            br.trim_preceding_space)==0)
                pre.resize(pre.size()-br.trim_preceding_space.size());
            if (!pre.empty()) state.fragments.push_back({std::move(pre), false});
        }
        std::string match = text.substr(bs, be-bs);
        if (br.is_protected && match.size()>1 && match[0]==' ') match = match.substr(1);
        if (br.behavior != "REMOVE") state.fragments.push_back({std::move(match), br.is_protected});
        last = be;
    }
    if (state.fragments.empty() && !text.empty())
        state.fragments.push_back({text, false});
}

// ── BPE_ENCODE (0x20) (Fully optimized - C++17 compatible) ────────────────────
static void op_bpe_encode(TISA_State& state, const X64Resources& res) {
    int32_t unk_id = 0;
    { auto it = res.vocab.find("<unk>"); if (it != res.vocab.end()) unk_id = it->second.id;
      else { auto it2 = res.vocab.find("[UNK]"); if (it2 != res.vocab.end()) unk_id = it2->second.id; } }
    
    state.ids.clear();
    if (state.fragments.empty() && !state.text.empty()) {
        state.fragments.push_back({state.text, false});
    }

    // Reusable string buffer to avoid allocations inside the loop (C++17 zero-alloc)
    std::string lookup_key;
    lookup_key.reserve(256);

    for (const auto& frag : state.fragments) {
        if (frag.is_protected) {
            int32_t id = unk_id;
            auto it = res.vocab.find(frag.text);
            if (it != res.vocab.end()) id = it->second.id;
            else if (frag.text.size() > 1 && frag.text[0] == ' ') {
                auto it2 = res.vocab.find(frag.text.substr(1));
                if (it2 != res.vocab.end()) id = it2->second.id;
            }
            state.ids.push_back(id);
            continue;
        }
        if (frag.text.empty()) continue;

        // Uses vector for contiguous memory (much better cache locality than std::list)
        std::vector<std::string> word;
        for (size_t off = 0; off < frag.text.size(); ) {
            size_t cl = get_utf8_char_len((unsigned char)frag.text[off]);
            if (!cl) { ++off; continue; }
            word.push_back(frag.text.substr(off, cl));
            off += cl;
        }

        if (word.empty()) continue;

        while (word.size() > 1) {
            int32_t min_rank = std::numeric_limits<int32_t>::max();
            size_t best_i = 0;

            for (size_t i = 0; i < word.size() - 1; ++i) {
                // Assign and append reusing existing capacity = zero allocation
                lookup_key.assign(word[i]);
                lookup_key.push_back('\0');
                lookup_key.append(word[i+1]);

                auto merge_it = res.merges.find(lookup_key);
                if (merge_it != res.merges.end() && merge_it->second < min_rank) {
                    min_rank = merge_it->second;
                    best_i = i;
                }
            }

            if (min_rank == std::numeric_limits<int32_t>::max()) break;

            std::string best_a = word[best_i];
            std::string best_b = word[best_i+1];

            // Rebuild vector in-place essentially (fastest for small arrays)
            std::vector<std::string> nw;
            nw.reserve(word.size());
            for (size_t i = 0; i < word.size(); ) {
                if (i < word.size() - 1 && word[i] == best_a && word[i+1] == best_b) {
                    nw.push_back(best_a + best_b);
                    i += 2;
                } else {
                    nw.push_back(std::move(word[i]));
                    i += 1;
                }
            }
            word = std::move(nw);
        }

        for (const auto& tok : word) {
            auto it = res.vocab.find(tok);
            state.ids.push_back(it != res.vocab.end() ? it->second.id : unk_id);
        }
    }
}

// ── WORDPIECE_ENCODE (0x21) ───────────────────────────────────────────────────
static void op_wordpiece_encode(TISA_State& state, const X64Resources& res, const std::string& marker) {
    int32_t unk_id = 100;
    { auto it=res.vocab.find("[UNK]"); if(it!=res.vocab.end()) unk_id=it->second.id; }
    state.ids.clear();
    for (const auto& frag : state.fragments) {
        if (frag.is_protected) {
            auto it=res.vocab.find(frag.text);
            state.ids.push_back(it!=res.vocab.end()?it->second.id:unk_id); continue;
        }
        const std::string& text=frag.text; if(text.empty()) continue;
        size_t start=0;
        while (start < text.length()) {
            size_t end=text.length(); bool found=false;
            while (start < end) {
                std::string sub=text.substr(start,end-start);
                std::string tok=(start==0)?sub:(marker+sub);
                auto it=res.vocab.find(tok);
                if (it!=res.vocab.end()) { state.ids.push_back(it->second.id); start=end; found=true; break; }
                if (end>start) { size_t pv=end-1; while(pv>start&&(text[pv]&0xC0)==0x80)--pv; end=pv; }
                else break;
            }
            if (!found) {
                state.ids.push_back(unk_id);
                size_t cl=get_utf8_char_len(text[start]); start+=(cl>0)?cl:1;
            }
        }
    }
}

// ── UNIGRAM_ENCODE (0x22) ─────────────────────────────────────────────────────
static void op_unigram_encode(TISA_State& state, const X64Resources& res) {
    int32_t unk_id = 2;
    { auto it=res.vocab.find("<unk>"); if(it!=res.vocab.end()) unk_id=it->second.id; }
    state.ids.clear();
    for (const auto& frag : state.fragments) {
        if (frag.is_protected) {
            auto it=res.vocab.find(frag.text);
            state.ids.push_back(it!=res.vocab.end()?it->second.id:unk_id); continue;
        }
        const std::string& text=frag.text; if(text.empty()) continue;
        std::vector<size_t> cp; for(size_t i=0;i<text.length();){ cp.push_back(i); size_t l=get_utf8_char_len(text[i]); i+=(l>0)?l:1; } cp.push_back(text.length());
        int n=(int)(cp.size()-1); if(!n) continue;
        std::vector<float> dp(n+1,-std::numeric_limits<float>::infinity());
        std::vector<int>   path(n+1,0); dp[0]=0.f;
        for (int i=0;i<n;++i) {
            if (dp[i]<=-std::numeric_limits<float>::infinity()/2) continue;
            for (int j=i+1;j<=n&&j<=i+50;++j) {
                std::string sub=text.substr(cp[i],cp[j]-cp[i]);
                auto it=res.vocab.find(sub);
                if (it!=res.vocab.end()&&dp[i]+it->second.score>dp[j]) { dp[j]=dp[i]+it->second.score; path[j]=i; }
            }
        }
        if (dp[n]<=-std::numeric_limits<float>::infinity()/2) {
            for(int i=0;i<n;++i){ std::string ch=text.substr(cp[i],cp[i+1]-cp[i]); auto it=res.vocab.find(ch); state.ids.push_back(it!=res.vocab.end()?it->second.id:unk_id); }
        } else {
            std::vector<int32_t> ids; int cur=n;
            while(cur>0){ int prev=path[cur]; std::string tok=text.substr(cp[prev],cp[cur]-cp[prev]); auto it=res.vocab.find(tok); ids.push_back(it!=res.vocab.end()?it->second.id:unk_id); cur=prev; }
            std::reverse(ids.begin(),ids.end()); state.ids.insert(state.ids.end(),ids.begin(),ids.end());
        }
    }
}

static std::vector<int32_t> tisa_run(const std::vector<uint8_t>& mf,
                                     const std::string& text,
                                     const X64Resources& res)
{
    if (mf.size() < 5 || memcmp(mf.data(), "TISA", 4) != 0) return {};
    TISA_State s; s.text = text;
    size_t off = 5;
    while (off < mf.size()) {
        if (off + 5 > mf.size()) break;
        uint8_t op = mf[off++];
        uint32_t len; memcpy(&len, &mf[off], 4); off += 4;
        if (len > mf.size() - off) break;
        dispatch_opcode(op, &mf[off], len, s, res);
        off += len;
    }
    return s.ids;
}

// =============================================================================
// TISADecoder  — Python-parity decode() (Fully Optimized)
// =============================================================================
enum class ModelKind : uint8_t { Unknown, BPE, WordPiece, Unigram };

static ModelKind detect_model_kind(const X64Resources& res,
                                   bool has_merges, bool has_unigram_scores)
{
    bool has_sp=false, has_wp=false;
    static const uint8_t SP[3]={0xe2,0x96,0x81};
    for (const auto& [tok, e] : res.vocab) {
        if (!has_sp && tok.size()>=3 && (uint8_t)tok[0]==SP[0]&&(uint8_t)tok[1]==SP[1]&&(uint8_t)tok[2]==SP[2]) has_sp=true;
        if (!has_wp && tok.size()>=2 && tok[0]=='#'&&tok[1]=='#') has_wp=true;
        if (has_sp && has_wp) break;
    }
    
    // Fix: Reverted to original logic!
    // Llama uses SentencePiece BPE (has_sp=true). In decoding, SP models must be
    // routed to Unigram (concat + replace \u2581), NOT Byte-Level BPE.
    if (has_sp)             return ModelKind::Unigram;
    if (has_merges)         return ModelKind::BPE;
    if (has_unigram_scores) return ModelKind::Unigram;
    if (has_wp)             return ModelKind::WordPiece;
    return ModelKind::Unknown;
}

static bool is_special_tok(std::string_view t) {
    // Fix: Require token size > 2 to prevent regular brackets "[]" from being skipped
    return t.size() > 2 && ((t.front()=='<'&&t.back()=='>')||(t.front()=='['&&t.back()==']'));
}

static std::string wp_postprocess(std::string s) {
    { std::string out; out.reserve(s.size());
      for (size_t i=0,n=s.size();i<n;) {
          if (s[i]==' ') {
              size_t j=i; while(j<n&&s[j]==' ')++j;
              const char puncts[]=".,!?\"'";
              if (j<n&&strchr(puncts,s[j])){i=j;continue;}
          }
          out+=s[i++];
      }
      s=std::move(out);
    }
    { std::string out; out.reserve(s.size());
      for (size_t i=0,n=s.size();i<n;)
          if (i+2<n&&s[i]==' '&&s[i+1]=='\''&&s[i+2]==' '){out+='\'';i+=3;}else out+=s[i++];
      s=std::move(out);
    }
    return s;
}

static std::string tisa_decode(const std::vector<int32_t>& ids,
                                const X64Resources& res,
                                ModelKind kind,
                                bool skip_special = true)
{
    std::vector<std::string_view> tokens;
    tokens.reserve(ids.size());
    for (int32_t id : ids) {
        std::string_view sv = token_view_by_id(res, id);
        if (sv.empty()) continue;
        if (skip_special && is_special_tok(sv)) continue;
        tokens.push_back(sv);
    }

    static const uint8_t SP3[3]={0xe2,0x96,0x81};

    switch (kind) {
    case ModelKind::BPE: {
        if (res.fast_rev_map.empty()) {
            std::string r;
            for (auto t:tokens){
                for (size_t i=0;i<t.size();)
                    if(i+1<t.size()&&(uint8_t)t[i]==0xC4&&(uint8_t)t[i+1]==0xA0){r+=' ';i+=2;}else r+=t[i++];
            }
            while(!r.empty()&&(uint8_t)r.front()<=0x20)r.erase(r.begin());
            while(!r.empty()&&(uint8_t)r.back()<=0x20)r.pop_back();
            return r;
        }

        std::vector<uint8_t> bytes; 
        bytes.reserve(tokens.size()*6);
        
        for (std::string_view tok : tokens) {
            for (size_t i=0; i<tok.size(); ) {
                size_t cl = get_utf8_char_len((unsigned char)tok[i]); 
                if (!cl) { ++i; continue; }
                if (i + cl > tok.size()) break;
                
                uint32_t cval = 0;
                memcpy(&cval, tok.data() + i, cl);
                
                auto it = res.fast_rev_map.find(cval);
                if (it != res.fast_rev_map.end()) {
                    bytes.push_back(it->second);
                }
                
                i += cl;
            }
        }
        
        std::string result; 
        result.reserve(bytes.size());
        for (size_t i=0, n=bytes.size(); i<n; ) {
            uint8_t b = bytes[i];
            size_t exp = (b<0x80)?1 : ((b&0xE0)==0xC0)?2 : ((b&0xF0)==0xE0)?3 : ((b&0xF8)==0xF0)?4 : 0;
            if (!exp || i + exp > n) { 
                result += "\xEF\xBF\xBD"; 
                ++i; 
                continue; 
            }
            for (size_t k=0; k<exp; ++k) result += (char)bytes[i+k];
            i += exp;
        }
        return result;
    }
    case ModelKind::WordPiece: {
        if (tokens.empty()) return {};
        std::string joined; joined.reserve(tokens.size()*8);
        for (size_t i=0;i<tokens.size();++i){if(i)joined+=' ';joined+=tokens[i];}
        { std::string out; out.reserve(joined.size());
          for(size_t i=0,n=joined.size();i<n;)
              if(i+2<n&&joined[i]==' '&&joined[i+1]=='#'&&joined[i+2]=='#'){i+=3;}else out+=joined[i++];
          joined=std::move(out); }
        return wp_postprocess(std::move(joined));
    }
    case ModelKind::Unigram: {
        std::string s; s.reserve(tokens.size()*6);
        for (auto t:tokens) s+=t;
        std::string out; out.reserve(s.size());
        for (size_t i=0;i<s.size();)
            if(i+2<s.size()&&(uint8_t)s[i]==SP3[0]&&(uint8_t)s[i+1]==SP3[1]&&(uint8_t)s[i+2]==SP3[2]){out+=' ';i+=3;}else out+=s[i++];
        while(!out.empty()&&(uint8_t)out.front()<=0x20)out.erase(out.begin());
        while(!out.empty()&&(uint8_t)out.back()<=0x20)out.pop_back();
        return out;
    }
    default: {
        std::string r; for(size_t i=0;i<tokens.size();++i){if(i)r+=' ';r+=tokens[i];} return r;
    }
    }
}

// =============================================================================
// Binary test suite reader
// =============================================================================
struct TestCase {
    std::string           model_id;
    std::string           text;
    std::vector<uint8_t>  manifest;
    std::vector<int32_t>  ref_ids;
};

static bool read_test_suite(const std::string& path, std::vector<TestCase>& cases) {
    auto raw = read_file_bytes(path);
    if (raw.size() < 8 || memcmp(raw.data(), "TSTS", 4) != 0) {
        fprintf(stderr, "ERROR: invalid test suite magic: %s\n", path.c_str());
        return false;
    }
    uint32_t count; memcpy(&count, raw.data()+4, 4);
    size_t pos = 8; cases.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        TestCase tc;
        if (pos+2 > raw.size()) break;
        uint16_t mid_len; memcpy(&mid_len, raw.data()+pos, 2); pos+=2;
        if (pos+mid_len > raw.size()) break;
        tc.model_id.assign((char*)raw.data()+pos, mid_len); pos+=mid_len;

        if (pos+4 > raw.size()) break;
        uint32_t txt_len; memcpy(&txt_len, raw.data()+pos, 4); pos+=4;
        if (pos+txt_len > raw.size()) break;
        tc.text.assign((char*)raw.data()+pos, txt_len); pos+=txt_len;

        if (pos+4 > raw.size()) break;
        uint32_t mf_len; memcpy(&mf_len, raw.data()+pos, 4); pos+=4;
        if (pos+mf_len > raw.size()) break;
        tc.manifest.assign(raw.data()+pos, raw.data()+pos+mf_len); pos+=mf_len;

        if (pos+4 > raw.size()) break;
        uint32_t id_count; memcpy(&id_count, raw.data()+pos, 4); pos+=4;
        if (pos+id_count*4 > raw.size()) break;
        tc.ref_ids.resize(id_count);
        memcpy(tc.ref_ids.data(), raw.data()+pos, id_count*4); pos+=id_count*4;

        cases.push_back(std::move(tc));
    }
    return true;
}

static std::map<std::string, std::shared_ptr<X64Resources>> g_res_cache;

static std::shared_ptr<X64Resources> load_model(const std::string& models_dir,
                                                  const std::string& hash)
{
    auto it = g_res_cache.find(hash);
    if (it != g_res_cache.end()) return it->second;

    auto res = std::make_shared<X64Resources>();
    std::string base = models_dir + "/" + hash + "/";

    if (!load_vocab    (base + "vocab.b",     *res)) { fprintf(stderr,"  WARN: vocab.b missing for %s\n",hash.c_str()); }
    if (!load_vocab_idx(base + "vocab_idx.b", *res)) { fprintf(stderr,"  WARN: vocab_idx.b missing for %s\n",hash.c_str()); }
    load_merges(base + "merges.b", *res);
    res->byte_map = build_byte_map();

    res->fast_rev_map.reserve(256);
    for (int i = 0; i < 256; ++i) {
        const auto& s = res->byte_map[i];
        if (!s.empty()) {
            uint32_t val = 0;
            memcpy(&val, s.data(), std::min<size_t>(4, s.length()));
            res->fast_rev_map[val] = static_cast<uint8_t>(i);
        }
    }

    printf("  [model] %s  vocab=%u  idx=%u  merges=%s\n",
           hash.c_str(),
           (unsigned)res->vocab.size(),
           (unsigned)res->vocab_idx.size(),
           res->merges_loaded ? "yes" : "no");
    g_res_cache[hash] = res;
    return res;
}

static std::map<std::string,std::string> load_model_map(const std::string& path) {
    std::map<std::string,std::string> m;
    std::ifstream f(path);
    if (!f) return m;
    std::string line;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string id   = line.substr(0, colon);
        std::string hash = line.substr(colon+1);
        while (!hash.empty() && (hash.back()=='\r'||hash.back()=='\n'||hash.back()==' ')) hash.pop_back();
        while (!id.empty()   && (id.back()==' '))   id.pop_back();
        if (!id.empty() && !hash.empty()) m[id] = hash;
    }
    return m;
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    std::string models_dir = "models";
    std::string suite_path = "tisa_test_suite.bin";
    if (argc >= 2) models_dir = argv[1];
    if (argc >= 3) suite_path = argv[2];

    printf("\n");
    printf("+================================================================+\n");
    printf("|           TISA VM x64 Test Suite                               |\n");
    printf("|           Copyright (c) 2026 Dmitry Feklin                     |\n");
    printf("+================================================================+\n");
    printf("\n");
    printf("[init] Models dir : %s\n", models_dir.c_str());
    printf("[init] Test suite : %s\n", suite_path.c_str());

    auto model_map = load_model_map(models_dir + "/model_map.txt");
    printf("[init] model_map  : %u entries\n\n", (unsigned)model_map.size());

    std::vector<TestCase> cases;
    if (!read_test_suite(suite_path, cases)) {
        fprintf(stderr, "FATAL: cannot read test suite.\n");
        return 1;
    }
    printf("[init] Test cases : %u\n\n", (unsigned)cases.size());

    uint32_t total=0, passed_enc=0, failed_enc=0, passed_dec=0, errors=0;

    for (size_t i = 0; i < cases.size(); ++i) {
        const TestCase& tc = cases[i];
        total++;

        printf("------------------------------------------------------------------\n");
        printf("Test %u/%u: %s\n", (unsigned)(i+1), (unsigned)cases.size(), tc.model_id.c_str());
        printf("Text: \"%s\"\n", tc.text.c_str());

        auto mit = model_map.find(tc.model_id);
        if (mit == model_map.end()) {
            printf("  [ERROR] model_id not in model_map.txt\n");
            ++errors; continue;
        }
        const std::string& hash = mit->second;

        auto res = load_model(models_dir, hash);
        if (!res || res->vocab.empty()) {
            printf("  [ERROR] failed to load model resources\n");
            ++errors; continue;
        }

        ModelKind kind = detect_model_kind(*res, res->merges_loaded, false);

        auto t0 = std::chrono::high_resolution_clock::now();
        auto vm_ids = tisa_run(tc.manifest, tc.text, *res);
        auto t1 = std::chrono::high_resolution_clock::now();
        double enc_us = std::chrono::duration<double,std::micro>(t1-t0).count();

        bool enc_ok = (vm_ids == tc.ref_ids);
        if (enc_ok) {
            printf("  [PASS] ENCODE  [%u tokens]  %.1f us\n", (unsigned)vm_ids.size(), enc_us);
            ++passed_enc;
        } else {
            printf("  [FAIL] ENCODE  vm=%u tokens  ref=%u tokens\n",
                   (unsigned)vm_ids.size(), (unsigned)tc.ref_ids.size());
            size_t maxl = std::max(vm_ids.size(), tc.ref_ids.size());
            for (size_t j=0;j<maxl&&j<5;++j) {
                int32_t vm_id  = (j<vm_ids.size())   ? vm_ids[j]   : -1;
                int32_t ref_id = (j<tc.ref_ids.size())? tc.ref_ids[j]: -1;
                if (vm_id!=ref_id) printf("  [%u] vm=%d ref=%d\n",(unsigned)j,vm_id,ref_id);
            }
            ++failed_enc;
        }

        auto t2 = std::chrono::high_resolution_clock::now();
        std::string decoded = tisa_decode(tc.ref_ids, *res, kind);
        auto t3 = std::chrono::high_resolution_clock::now();
        double dec_us = std::chrono::duration<double,std::micro>(t3-t2).count();

        printf("         DECODE  '%s'  %.1f us\n", decoded.c_str(), dec_us);

        // Fix: Restored original logic. Uncased models change the case, so exact match fails.
        // If it decodes *something* reasonably, we consider the decode path fully functional.
        bool rt_ok = (decoded == tc.text); // || !decoded.empty()
        if (rt_ok) ++passed_dec;
    }

    printf("\n");
    printf("+================================================================+\n");
    printf("|                       TEST SUMMARY                             |\n");
    printf("+================================================================+\n");
    printf("|  Total          : %3u                                          |\n", total);
    printf("|  Encode PASS    : %3u  (%.1f%%)                                |\n", passed_enc, total?100.0*passed_enc/total:0.0);
    printf("|  Encode FAIL    : %3u  (%.1f%%)                                |\n", failed_enc, total?100.0*failed_enc/total:0.0);
    printf("|  Decode PASS    : %3u                                          |\n", passed_dec);
    printf("|  Errors         : %3u                                          |\n", errors);
    printf("+================================================================+\n");

    if (!g_res_cache.empty()) {
        printf("\n[bench] Warming up with last loaded model...\n");
        const auto& [bh, bres] = *g_res_cache.rbegin();
        ModelKind bk = detect_model_kind(*bres, bres->merges_loaded, false);

        const TestCase* btc = nullptr;
        for (const auto& tc : cases) {
            auto mit=model_map.find(tc.model_id);
            if (mit!=model_map.end()&&mit->second==bh) { btc=&tc; break; }
        }
        if (btc) {
            const int N = 10000;
            auto b0 = std::chrono::high_resolution_clock::now();
            for (int i=0;i<N;++i) tisa_run(btc->manifest, btc->text, *bres);
            auto b1 = std::chrono::high_resolution_clock::now();
            double enc_ms = std::chrono::duration<double,std::milli>(b1-b0).count();

            b0 = std::chrono::high_resolution_clock::now();
            for (int i=0;i<N;++i) tisa_decode(btc->ref_ids, *bres, bk);
            b1 = std::chrono::high_resolution_clock::now();
            double dec_ms = std::chrono::duration<double,std::milli>(b1-b0).count();

            printf("[bench] Model: %s | %s\n", btc->model_id.c_str(), bh.c_str());
            printf("[bench] Encode: %.1f ms / %d iters = %.0f ns/call\n", enc_ms, N, enc_ms*1e6/N);
            printf("[bench] Decode: %.1f ms / %d iters = %.0f ns/call\n", dec_ms, N, dec_ms*1e6/N);
        }
    }

    printf("\n");
    return (failed_enc > 0 || errors > 0) ? 1 : 0;
}