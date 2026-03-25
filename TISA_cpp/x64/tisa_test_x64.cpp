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

static bool is_category(uint32_t cp, std::string_view cat) {
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

static std::string unescape_pattern(std::string_view p) {
    std::string r; r.reserve(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\\' && i + 1 < p.size()) { r += p[++i]; }
        else                                  { r += p[i]; }
    }
    return r;
}

struct Rule {
    std::string_view pattern;
    bool             is_protected = false;
    std::string_view behavior;
    std::string_view trim_preceding_space;

    // Предварительно разобранные свойства паттернов для быстрого мэтчинга
    std::string unescaped_lit;
    bool is_opt_space_lit = false;
    bool is_punct = false;
    bool is_space = false;
    bool is_cjk = false;
    bool is_gpt = false;
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
        std::string result; result.reserve(s.text.length());
        for (size_t i = 0; i < s.text.length(); ) {
            size_t cl = get_utf8_char_len(s.text[i]); if (!cl){ result += s.text[i++]; continue; }
            uint32_t cp = utf8_to_codepoint((const unsigned char*)&s.text[i], cl);
            uint32_t lo = cp;
            { int a=0,b=(int)exc_count-1;
              while(a<=b){int m=a+(b-a)/2; uint32_t f=pgm_read_dword(&LOWERCASE_EXCEPTIONS[m].from);
                if(cp==f){lo=pgm_read_dword(&LOWERCASE_EXCEPTIONS[m].to);break;}
                else if(cp<f)b=m-1;else a=m+1;}}
            if (lo==cp){int a=0,b=(int)rng_count-1;
              while(a<=b){int m=a+(b-a)/2;uint32_t ss=pgm_read_dword(&LOWERCASE_RANGES[m].start),ee=pgm_read_dword(&LOWERCASE_RANGES[m].end);
                if(cp<ss)b=m-1;else if(cp>ee)a=m+1;else{lo=(uint32_t)((int32_t)cp+(int32_t)pgm_read_dword(&LOWERCASE_RANGES[m].delta));break;}}}
            
            if (lo == cp) result.append(s.text, i, cl);
            else result += codepoint_to_utf8(lo);
            i += cl;
        }
        s.text = std::move(result);
        break;
    }
    case 0x02: {
        std::string_view form((const char*)&p[1], p[0]);
        if (form == "NFD") {
            const size_t dc = sizeof(DECOMP_TABLE)/sizeof(Decomp);
            std::string decomposed; decomposed.reserve(s.text.length() * 2);
            for (size_t i = 0; i < s.text.length(); ) {
                size_t cl = get_utf8_char_len(s.text[i]); if(!cl){ decomposed += s.text[i++]; continue; }
                uint32_t cp = utf8_to_codepoint((const unsigned char*)&s.text[i], cl);
                int a=0,b=(int)dc-1; bool found=false;
                while(a<=b){int m=a+(b-a)/2; uint32_t f=pgm_read_dword(&DECOMP_TABLE[m].from);
                  if(cp==f){decomposed+=codepoint_to_utf8(pgm_read_dword(&DECOMP_TABLE[m].to1));
                    uint32_t t2=pgm_read_dword(&DECOMP_TABLE[m].to2); if(t2)decomposed+=codepoint_to_utf8(t2);
                    found=true;break;}else if(cp<f)b=m-1;else a=m+1;}
                if(!found) decomposed.append(s.text, i, cl);
                i += cl;
            }
            s.text = std::move(decomposed);
        }
        break;
    }
    case 0x03: {
        uint16_t pat_len; memcpy(&pat_len, &p[0], 2);
        std::string_view pat((const char*)&p[2], pat_len);
        uint16_t val_len; memcpy(&val_len, &p[2 + pat_len], 2);
        std::string_view val((const char*)&p[4 + pat_len], val_len);
        
        std::string res_str; res_str.reserve(s.text.size());
        size_t last = 0, pos;
        while ((pos = s.text.find(pat, last)) != std::string::npos) {
            res_str.append(s.text, last, pos - last);
            res_str.append(val);
            last = pos + pat.size();
        }
        res_str.append(s.text, last, std::string::npos);
        s.text = std::move(res_str);
        break;
    }
    case 0x04: {
        uint8_t nc = p[0]; const uint8_t* ptr = &p[1];
        std::vector<std::string_view> cats; cats.reserve(nc);
        for (int i=0;i<nc;++i){ uint8_t l=*ptr++; cats.push_back({(const char*)ptr,l}); ptr+=l; }
        std::string out; out.reserve(s.text.length());
        for (size_t i=0;i<s.text.length();) {
            size_t cl=get_utf8_char_len(s.text[i]); if(!cl){out+=s.text[i++];continue;}
            uint32_t cp=utf8_to_codepoint((const unsigned char*)&s.text[i],cl);
            bool filt=false; for(auto&c:cats) if(is_category(cp, c)){filt=true;break;}
            if(!filt) out.append(s.text, i, cl);
            i+=cl;
        }
        s.text=std::move(out);
        break;
    }
    case 0x07: {
        uint16_t val_len; memcpy(&val_len, &p[0], 2);
        std::string_view val((const char*)&p[2], val_len);
        if (s.text.compare(0, val.size(), val) != 0) s.text.insert(0, val.data(), val.size());
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
        std::vector<int32_t> out; out.reserve(s.ids.size() + p[0]);
        uint8_t n = p[0]; const uint8_t* ptr = &p[1];
        for (int i=0;i<n;++i) {
            if (ptr[0]) {
                ptr++;
                if (ptr[0]) {
                    ptr++; int32_t id; memcpy(&id,ptr,4); ptr+=4; out.push_back(id);
                } else {
                    ptr++; std::string_view tok((const char*)&ptr[1],ptr[0]); ptr+=1+tok.size();
                    auto it=res.vocab.find(std::string(tok));
                    out.push_back(it!=res.vocab.end() ? it->second.id : 2);
                }
            } else {
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
        Rule r;
        uint8_t flags = *ptr++;
        r.is_protected = (flags & 1);
        uint16_t pl; memcpy(&pl, ptr, 2); ptr += 2;
        r.pattern = std::string_view((const char*)ptr, pl); ptr += pl;
        if (flags & 2) { uint8_t b=*ptr++; r.behavior=(b==1)?"REMOVE":(b==2?"ISOLATE":""); }
        if (flags & 4) { uint8_t tl=*ptr++; r.trim_preceding_space=std::string_view((const char*)ptr,tl); ptr+=tl; }
        
        if (r.pattern.rfind("(?: ?)", 0) == 0) {
            r.unescaped_lit = unescape_pattern(r.pattern.substr(6));
            r.is_opt_space_lit = true;
        } else if (r.pattern == "\\p{P}") {
            r.is_punct = true;
        } else if (r.pattern == "\\s+") {
            r.is_space = true;
        } else if (r.pattern.find("\xe4\xb8\x80")!=std::string_view::npos || r.pattern.find("\\u4E00")!=std::string_view::npos) {
            r.is_cjk = true;
        } else if (r.pattern.find("'s|'t|'re") != std::string_view::npos) {
            r.is_gpt = true;
        } else {
            r.unescaped_lit = unescape_pattern(r.pattern);
        }
        rules.push_back(std::move(r));
    }

    size_t last = 0;
    while (last < text.length()) {
        size_t bs = std::string::npos, be = 0; int bi = -1;
        for (size_t i = 0; i < rules.size(); ++i) {
            const Rule& rule = rules[i];
            size_t ms = std::string::npos, me = 0;

            if (rule.is_opt_space_lit) {
                size_t p1 = text.find(rule.unescaped_lit, last);
                if (p1 != std::string::npos) {
                    if (p1 > last && text[p1 - 1] == ' ') { ms = p1 - 1; } 
                    else { ms = p1; }
                    me = p1 + rule.unescaped_lit.size();
                }
            } else if (rule.is_punct) {
                for (size_t k = last; k < text.length(); ) {
                    size_t cl = get_utf8_char_len(text[k]); if(!cl){++k;continue;}
                    uint32_t cp = utf8_to_codepoint((const unsigned char*)&text[k], cl);
                    if ((cp=='.'||cp==','||cp=='!'||cp=='?'||cp==':'||cp==';'||cp=='"'||cp=='\''||
                         cp=='('||cp==')'||cp=='['||cp==']'||cp=='{'||cp=='}')||is_category(cp,"P"))
                        { ms=k; me=k+cl; break; }
                    k += cl;
                }
            } else if (rule.is_space) {
                for (size_t k = last; k < text.length(); ) {
                    size_t cl = get_utf8_char_len(text[k]); if(!cl){++k;continue;}
                    if (is_whitespace(utf8_to_codepoint((const unsigned char*)&text[k],cl))) {
                        ms = k; size_t e = k + cl;
                        while(e < text.length()){
                            size_t nl=get_utf8_char_len(text[e]); 
                            if(!nl||!is_whitespace(utf8_to_codepoint((const unsigned char*)&text[e],nl)))break; 
                            e+=nl;
                        }
                        me = e; break;
                    }
                    k += cl;
                }
            } else if (rule.is_cjk) {
                for (size_t k = last; k < text.length(); ) {
                    size_t cl = get_utf8_char_len(text[k]); if(!cl){++k;continue;}
                    if (is_cjk(utf8_to_codepoint((const unsigned char*)&text[k],cl))) { ms=k; me=k+cl; break; }
                    k += cl;
                }
            } else if (rule.is_gpt) {
                ms = find_gpt_next_token(text, last, me);
            } else {
                if (!rule.unescaped_lit.empty()) {
                    size_t p = text.find(rule.unescaped_lit, last);
                    if (p != std::string::npos) { ms = p; me = p + rule.unescaped_lit.size(); }
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
                pre.compare(pre.size()-br.trim_preceding_space.size(), br.trim_preceding_space.size(), br.trim_preceding_space)==0)
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

// Узел виртуального связного списка
struct BPESymbol {
    int prev, next;
    size_t start, len;
};

// ── BPE_ENCODE (0x20) (Fully optimized - C++17 compatible) ────────────────────
static void op_bpe_encode(TISA_State& state, const X64Resources& res) {
    int32_t unk_id = 0;
    { auto it = res.vocab.find("<unk>"); if (it != res.vocab.end()) unk_id = it->second.id;
      else { auto it2 = res.vocab.find("[UNK]"); if (it2 != res.vocab.end()) unk_id = it2->second.id; } }
    
    state.ids.clear();
    if (state.fragments.empty() && !state.text.empty()) {
        state.fragments.push_back({state.text, false});
    }

    std::string lookup_key; lookup_key.reserve(256);
    std::vector<BPESymbol> syms;

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

        syms.clear();
        syms.reserve(frag.text.size());

        int prev_idx = -1;
        for (size_t off = 0; off < frag.text.size(); ) {
            size_t cl = get_utf8_char_len((unsigned char)frag.text[off]);
            if (!cl) { ++off; continue; }
            syms.push_back({prev_idx, -1, off, cl});
            int cur_idx = (int)syms.size() - 1;
            if (prev_idx != -1) syms[prev_idx].next = cur_idx;
            prev_idx = cur_idx;
            off += cl;
        }

        if (syms.empty()) continue;
        int head = 0;

        while (true) {
            int32_t min_rank = std::numeric_limits<int32_t>::max();
            int best_i = -1;

            // O(N) проход только по актуальным несмерженным узлам за счет связного списка
            for (int i = head; i != -1; i = syms[i].next) {
                int nxt = syms[i].next;
                if (nxt == -1) break;

                // Переиспользуем один буфер без аллокаций
                lookup_key.clear();
                lookup_key.append(frag.text, syms[i].start, syms[i].len);
                lookup_key.push_back('\0');
                lookup_key.append(frag.text, syms[nxt].start, syms[nxt].len);

                auto merge_it = res.merges.find(lookup_key);
                if (merge_it != res.merges.end() && merge_it->second < min_rank) {
                    min_rank = merge_it->second;
                    best_i = i;
                }
            }

            if (best_i == -1) break;

            // Выполняем O(1) слияние (переброс ссылок, длина увеличивается)
            int nxt = syms[best_i].next;
            syms[best_i].len += syms[nxt].len;
            syms[best_i].next = syms[nxt].next;
            if (syms[nxt].next != -1) {
                syms[syms[nxt].next].prev = best_i;
            }
        }

        for (int i = head; i != -1; i = syms[i].next) {
            lookup_key.assign(frag.text, syms[i].start, syms[i].len);
            auto it = res.vocab.find(lookup_key);
            state.ids.push_back(it != res.vocab.end() ? it->second.id : unk_id);
        }
    }
}

// ── WORDPIECE_ENCODE (0x21) ───────────────────────────────────────────────────
static void op_wordpiece_encode(TISA_State& state, const X64Resources& res, const std::string& marker) {
    int32_t unk_id = 100;
    { auto it=res.vocab.find("[UNK]"); if(it!=res.vocab.end()) unk_id=it->second.id; }
    state.ids.clear();
    std::string lookup_key; lookup_key.reserve(256);
    
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
                lookup_key.clear();
                if (start > 0) lookup_key.append(marker);
                lookup_key.append(text, start, end - start);
                
                auto it = res.vocab.find(lookup_key);
                if (it != res.vocab.end()) { state.ids.push_back(it->second.id); start=end; found=true; break; }
                if (end>start) { size_t pv=end-1; while(pv>start&&(text[pv]&0xC0)==0x80)--pv; end=pv; }
                else break;
            }
            if (!found) {
                state.ids.push_back(unk_id);
                size_t cl=get_utf8_char_len((unsigned char)text[start]); start+=(cl>0)?cl:1;
            }
        }
    }
}

// ── UNIGRAM_ENCODE (0x22) ─────────────────────────────────────────────────────
static void op_unigram_encode(TISA_State& state, const X64Resources& res) {
    int32_t unk_id = 2;
    { auto it=res.vocab.find("<unk>"); if(it!=res.vocab.end()) unk_id=it->second.id; }
    state.ids.clear();
    std::string lookup_key; lookup_key.reserve(256);
    
    for (const auto& frag : state.fragments) {
        if (frag.is_protected) {
            auto it=res.vocab.find(frag.text);
            state.ids.push_back(it!=res.vocab.end()?it->second.id:unk_id); continue;
        }
        const std::string& text=frag.text; if(text.empty()) continue;
        std::vector<size_t> cp; for(size_t i=0;i<text.length();){ cp.push_back(i); size_t l=get_utf8_char_len((unsigned char)text[i]); i+=(l>0)?l:1; } cp.push_back(text.length());
        int n=(int)(cp.size()-1); if(!n) continue;
        std::vector<float> dp(n+1,-std::numeric_limits<float>::infinity());
        std::vector<int>   path(n+1,0); dp[0]=0.f;
        for (int i=0;i<n;++i) {
            if (dp[i]<=-std::numeric_limits<float>::infinity()/2) continue;
            for (int j=i+1;j<=n&&j<=i+50;++j) {
                lookup_key.assign(text, cp[i], cp[j] - cp[i]);
                auto it = res.vocab.find(lookup_key);
                if (it!=res.vocab.end()&&dp[i]+it->second.score>dp[j]) { dp[j]=dp[i]+it->second.score; path[j]=i; }
            }
        }
        if (dp[n]<=-std::numeric_limits<float>::infinity()/2) {
            for(int i=0;i<n;++i){
                lookup_key.assign(text, cp[i], cp[i+1]-cp[i]);
                auto it=res.vocab.find(lookup_key);
                state.ids.push_back(it!=res.vocab.end()?it->second.id:unk_id);
            }
        } else {
            std::vector<int32_t> ids; int cur=n;
            while(cur>0){
                int prev=path[cur];
                lookup_key.assign(text, cp[prev], cp[cur]-cp[prev]);
                auto it=res.vocab.find(lookup_key);
                ids.push_back(it!=res.vocab.end()?it->second.id:unk_id); cur=prev;
            }
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

static std::string wp_postprocess(std::string_view s) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0, n = s.size(); i < n;) {
        if (s[i] == ' ') {
            size_t j = i; while (j < n && s[j] == ' ') ++j;
            const char puncts[] = ".,!?\"'";
            if (j < n && strchr(puncts, s[j])) { i = j; continue; }
        }
        out += s[i++];
    }
    std::string out2; out2.reserve(out.size());
    for (size_t i = 0, n = out.size(); i < n;) {
        if (i + 2 < n && out[i] == ' ' && out[i + 1] == '\'' && out[i + 2] == ' ') {
            out2 += '\''; i += 3;
        } else {
            out2 += out[i++];
        }
    }
    return out2;
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
            std::string r; r.reserve(tokens.size() * 8);
            for (auto t:tokens){
                for (size_t i=0;i<t.size();) {
                    if(i+1<t.size()&&(uint8_t)t[i]==0xC4&&(uint8_t)t[i+1]==0xA0){r+=' ';i+=2;}
                    else r+=t[i++];
                }
            }
            size_t start = 0;
            while(start < r.size() && (uint8_t)r[start] <= 0x20) start++;
            size_t end = r.size();
            while(end > start && (uint8_t)r[end-1] <= 0x20) end--;
            return r.substr(start, end - start);
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
            result.append((const char*)&bytes[i], exp);
            i += exp;
        }
        return result;
    }
    case ModelKind::WordPiece: {
        if (tokens.empty()) return {};
        std::string out; out.reserve(tokens.size() * 8);
        for (size_t i=0; i<tokens.size(); ++i) {
            std::string_view t = tokens[i];
            if (i > 0) {
                if (t.size() >= 2 && t[0] == '#' && t[1] == '#') {
                    t = t.substr(2);
                } else {
                    out += ' ';
                }
            }
            out += t;
        }
        return wp_postprocess(out);
    }
    case ModelKind::Unigram: {
        std::string s; s.reserve(tokens.size()*6);
        for (auto t:tokens) s+=t;
        std::string out; out.reserve(s.size());
        for (size_t i=0;i<s.size();) {
            if(i+2<s.size()&&(uint8_t)s[i]==SP3[0]&&(uint8_t)s[i+1]==SP3[1]&&(uint8_t)s[i+2]==SP3[2]){out+=' ';i+=3;}
            else out+=s[i++];
        }
        size_t start = 0;
        while(start < out.size() && (uint8_t)out[start] <= 0x20) start++;
        size_t end = out.size();
        while(end > start && (uint8_t)out[end-1] <= 0x20) end--;
        return out.substr(start, end - start);
    }
    default: {
        std::string r; r.reserve(tokens.size()*8);
        for(size_t i=0;i<tokens.size();++i){if(i)r+=' ';r+=tokens[i];} return r;
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

    bool bench_mode = false;

    std::string models_dir = "models";
    std::string suite_path = "tisa_test_suite.bin";

    // ---- parse args ----
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bench") {
            bench_mode = true;
        } else if (i == 1) {
            models_dir = arg;
        } else if (i == 2) {
            suite_path = arg;
        }
    }

    printf("\n");
    printf("+================================================================+\n");
    printf("|           TISA VM x64 Test Suite                               |\n");
    printf("|           Copyright (c) 2026 Dmitry Feklin                     |\n");
    printf("+================================================================+\n");
    printf("\n");

    if (bench_mode) {
        printf("[mode] BENCH ENABLED (1000 iterations per model)\n");
    }

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

    struct ModelStats {
        uint32_t tests = 0;
        uint32_t pass = 0;
        uint32_t fail = 0;
        double enc_time_us = 0.0;
        double dec_time_us = 0.0;

        // bench stats
        double enc_min = 1e30;
        double enc_max = 0.0;
        double enc_sum = 0.0;
        uint32_t enc_iters = 0;

        double dec_min = 1e30;
        double dec_max = 0.0;
        double dec_sum = 0.0;
        uint32_t dec_iters = 0;
    };

    std::unordered_map<std::string, ModelStats> stats;

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

        ModelStats& st = stats[tc.model_id];
        st.tests++;

        // ---------------------------
        // BENCH MODE
        // ---------------------------
        if (bench_mode) {
            const int N = 1000;

            // encode bench
            std::vector<double> enc_times;
            enc_times.reserve(N);

            std::vector<double> dec_times;
            dec_times.reserve(N);

            for (int i = 0; i < N; ++i) {
                auto t0 = std::chrono::high_resolution_clock::now();
                auto vm_ids = tisa_run(tc.manifest, tc.text, *res);
                auto t1 = std::chrono::high_resolution_clock::now();

                double enc_us = std::chrono::duration<double,std::micro>(t1-t0).count();
                enc_times.push_back(enc_us);

                auto t2 = std::chrono::high_resolution_clock::now();
                std::string decoded = tisa_decode(tc.ref_ids, *res, kind);
                auto t3 = std::chrono::high_resolution_clock::now();

                double dec_us = std::chrono::duration<double,std::micro>(t3-t2).count();
                dec_times.push_back(dec_us);
            }

            auto compute_stats = [](const std::vector<double>& v,
                                    double& min_v,
                                    double& max_v,
                                    double& avg_v)
            {
                min_v = 1e30;
                max_v = 0.0;
                double sum = 0.0;

                for (double x : v) {
                    if (x < min_v) min_v = x;
                    if (x > max_v) max_v = x;
                    sum += x;
                }
                avg_v = sum / v.size();
            };

            double enc_min, enc_max, enc_avg;
            double dec_min, dec_max, dec_avg;

            compute_stats(enc_times, enc_min, enc_max, enc_avg);
            compute_stats(dec_times, dec_min, dec_max, dec_avg);

            st.enc_min = enc_min;
            st.enc_max = enc_max;
            st.enc_sum += enc_avg * N;
            st.enc_iters += N;

            st.dec_min = dec_min;
            st.dec_max = dec_max;
            st.dec_sum += dec_avg * N;
            st.dec_iters += N;

            printf("  [BENCH] ENC avg=%.2f us min=%.2f max=%.2f\n", enc_avg, enc_min, enc_max);
            printf("  [BENCH] DEC avg=%.2f us min=%.2f max=%.2f\n", dec_avg, dec_min, dec_max);

        } else {
            // ---------------------------
            // NORMAL MODE
            // ---------------------------
            auto t0 = std::chrono::high_resolution_clock::now();
            auto vm_ids = tisa_run(tc.manifest, tc.text, *res);
            auto t1 = std::chrono::high_resolution_clock::now();
            double enc_us = std::chrono::duration<double,std::micro>(t1-t0).count();

            bool enc_ok = (vm_ids == tc.ref_ids);

            if (enc_ok) {
                printf("  [PASS] ENCODE  [%u tokens]  %.1f us\n", (unsigned)vm_ids.size(), enc_us);
                ++passed_enc;
                st.pass++;
            } else {
                printf("  [FAIL] ENCODE  vm=%u tokens  ref=%u tokens\n",
                       (unsigned)vm_ids.size(), (unsigned)tc.ref_ids.size());
                ++failed_enc;
                st.fail++;
            }

            auto t2 = std::chrono::high_resolution_clock::now();
            std::string decoded = tisa_decode(tc.ref_ids, *res, kind);
            auto t3 = std::chrono::high_resolution_clock::now();
            double dec_us = std::chrono::duration<double,std::micro>(t3-t2).count();

            printf("         DECODE  '%s'  %.1f us\n", decoded.c_str(), dec_us);

            st.enc_time_us += enc_us;
            st.dec_time_us += dec_us;

            bool rt_ok = (decoded == tc.text);
            if (rt_ok) ++passed_dec;
        }
    }

    // ---------------------------
    // SUMMARY
    // ---------------------------
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

    // ---------------------------
    // MODEL TABLE
    // ---------------------------
    printf("\n+================================================================+\n");
    printf("|                    MODEL PERFORMANCE TABLE                     |\n");
    printf("+================================================================+\n");

    if (bench_mode) {
        printf("| %-47s | %-5s | %-5s | %-10s | %-10s | %-10s |\n",
               "Model", "PASS", "FAIL", "ENC(avg)", "ENC(min)", "ENC(max)");
        printf("+----------------------------------------------------------------+\n");

        for (const auto& [model_id, st] : stats) {
            double avg_enc = st.enc_iters ? st.enc_sum / st.enc_iters : 0.0;

            printf("| %-47s | %-5u | %-5u | %-10.2f | %-10.2f | %-10.2f |\n",
                   model_id.c_str(),
                   st.pass,
                   st.fail,
                   avg_enc,
                   st.enc_min,
                   st.enc_max);
        }

    } else {
        printf("| %-47s | %-5s | %-5s | %-10s | %-10s |\n",
               "Model", "PASS", "FAIL", "ENC(us)", "DEC(us)");
        printf("+----------------------------------------------------------------+\n");

        for (const auto& [model_id, st] : stats) {
            double avg_enc = st.tests ? st.enc_time_us / st.tests : 0.0;
            double avg_dec = st.tests ? st.dec_time_us / st.tests : 0.0;

            printf("| %-47s | %-5u | %-5u | %-10.1f | %-10.1f |\n",
                   model_id.c_str(),
                   st.pass,
                   st.fail,
                   avg_enc,
                   avg_dec);
        }
    }

    printf("+================================================================+\n");

    return (failed_enc > 0 || errors > 0) ? 1 : 0;
}