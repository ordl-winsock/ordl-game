/*
 * ORDL UI — Input Method Editor (IME)
 * Pure C23, zero external dependencies.
 * Supports Japanese (romaji→hiragana), Chinese (pinyin→hanzi), Korean (jamo).
 */

#include "forge/ui/ordl_ui.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ime_strcpy is available for future use */
#define IME_PREEDIT_CAP     64
#define IME_COMMIT_CAP      64
#define IME_CANDIDATE_CAP   16
#define IME_CANDIDATE_LEN   16

typedef struct {
    char preedit[IME_PREEDIT_CAP];
    size_t preedit_len;

    char committed[IME_COMMIT_CAP];
    size_t committed_len;

    char candidates[IME_CANDIDATE_CAP][IME_CANDIDATE_LEN];
    int candidate_count;
    int selected;

    bool active;
} ui_ime_state_t;

static ui_ime_state_t g_ime;

/* -------------------------------------------------------------------------- */
/* UTF-8 encode one Unicode codepoint                                         */
/* -------------------------------------------------------------------------- */
static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/* -------------------------------------------------------------------------- */
/* Japanese romaji → hiragana (longest-match)                                 */
/* -------------------------------------------------------------------------- */
typedef struct {
    const char *romaji;
    const char *hiragana; /* UTF-8 string */
} ja_entry_t;

/* Table ordered longest-first for greedy matching */
static const ja_entry_t ja_table[] = {
    /* yoon compounds */
    {"kya", "\xe3\x81\x8d\xe3\x82\x83"}, {"kyu", "\xe3\x81\x8d\xe3\x82\x85"}, {"kyo", "\xe3\x81\x8d\xe3\x82\x87"},
    {"sha", "\xe3\x81\x97\xe3\x82\x83"}, {"shu", "\xe3\x81\x97\xe3\x82\x85"}, {"sho", "\xe3\x81\x97\xe3\x82\x87"},
    {"cha", "\xe3\x81\xa1\xe3\x82\x83"}, {"chu", "\xe3\x81\xa1\xe3\x82\x85"}, {"cho", "\xe3\x81\xa1\xe3\x82\x87"},
    {"nya", "\xe3\x81\xab\xe3\x82\x83"}, {"nyu", "\xe3\x81\xab\xe3\x82\x85"}, {"nyo", "\xe3\x81\xab\xe3\x82\x87"},
    {"hya", "\xe3\x81\xb2\xe3\x82\x83"}, {"hyu", "\xe3\x81\xb2\xe3\x82\x85"}, {"hyo", "\xe3\x81\xb2\xe3\x82\x87"},
    {"mya", "\xe3\x81\xbf\xe3\x82\x83"}, {"myu", "\xe3\x81\xbf\xe3\x82\x85"}, {"myo", "\xe3\x81\xbf\xe3\x82\x87"},
    {"rya", "\xe3\x82\x8a\xe3\x82\x83"}, {"ryu", "\xe3\x82\x8a\xe3\x82\x85"}, {"ryo", "\xe3\x82\x8a\xe3\x82\x87"},
    {"gya", "\xe3\x81\x8e\xe3\x82\x83"}, {"gyu", "\xe3\x81\x8e\xe3\x82\x85"}, {"gyo", "\xe3\x81\x8e\xe3\x82\x87"},
    {"ja",  "\xe3\x81\x98\xe3\x82\x83"}, {"ju",  "\xe3\x81\x98\xe3\x82\x85"}, {"jo",  "\xe3\x81\x98\xe3\x82\x87"},
    {"bya", "\xe3\x81\xb3\xe3\x82\x83"}, {"byu", "\xe3\x81\xb3\xe3\x82\x85"}, {"byo", "\xe3\x81\xb3\xe3\x82\x87"},
    {"pya", "\xe3\x81\xb4\xe3\x82\x83"}, {"pyu", "\xe3\x81\xb4\xe3\x82\x85"}, {"pyo", "\xe3\x81\xb4\xe3\x82\x87"},
    /* basic */
    {"ka", "\xe3\x81\x8b"}, {"ki", "\xe3\x81\x8d"}, {"ku", "\xe3\x81\x8f"}, {"ke", "\xe3\x81\x91"}, {"ko", "\xe3\x81\x93"},
    {"sa", "\xe3\x81\x95"}, {"shi","\xe3\x81\x97"}, {"su", "\xe3\x81\x99"}, {"se", "\xe3\x81\x9b"}, {"so", "\xe3\x81\x9d"},
    {"ta", "\xe3\x81\x9f"}, {"chi","\xe3\x81\xa1"}, {"tsu","\xe3\x81\xa4"}, {"te", "\xe3\x81\xa6"}, {"to", "\xe3\x81\xa8"},
    {"na", "\xe3\x81\xaa"}, {"ni", "\xe3\x81\xab"}, {"nu", "\xe3\x81\xac"}, {"ne", "\xe3\x81\xad"}, {"no", "\xe3\x81\xae"},
    {"ha", "\xe3\x81\xaf"}, {"hi", "\xe3\x81\xb2"}, {"fu", "\xe3\x81\xb5"}, {"he", "\xe3\x81\xb8"}, {"ho", "\xe3\x81\xbb"},
    {"ma", "\xe3\x81\xbe"}, {"mi", "\xe3\x81\xbf"}, {"mu", "\xe3\x82\x80"}, {"me", "\xe3\x82\x81"}, {"mo", "\xe3\x82\x82"},
    {"ya", "\xe3\x82\x84"}, {"yu", "\xe3\x82\x86"}, {"yo", "\xe3\x82\x88"},
    {"ra", "\xe3\x82\x89"}, {"ri", "\xe3\x82\x8a"}, {"ru", "\xe3\x82\x8b"}, {"re", "\xe3\x82\x8c"}, {"ro", "\xe3\x82\x8d"},
    {"wa", "\xe3\x82\x8f"}, {"wo", "\xe3\x82\x92"}, {"nn", "\xe3\x82\x93"}, {"n",  "\xe3\x82\x93"},
    {"ga", "\xe3\x81\x8c"}, {"gi", "\xe3\x81\x8e"}, {"gu", "\xe3\x81\x90"}, {"ge", "\xe3\x81\x92"}, {"go", "\xe3\x81\x94"},
    {"za", "\xe3\x81\x96"}, {"ji", "\xe3\x81\x98"}, {"zu", "\xe3\x81\x9a"}, {"ze", "\xe3\x81\x9c"}, {"zo", "\xe3\x81\x9e"},
    {"da", "\xe3\x81\xa0"}, {"di", "\xe3\x81\xa2"}, {"du", "\xe3\x81\xa5"}, {"de", "\xe3\x81\xa7"}, {"do", "\xe3\x81\xa9"},
    {"ba", "\xe3\x81\xb0"}, {"bi", "\xe3\x81\xb3"}, {"bu", "\xe3\x81\xb6"}, {"be", "\xe3\x81\xb9"}, {"bo", "\xe3\x81\xbc"},
    {"pa", "\xe3\x81\xb1"}, {"pi", "\xe3\x81\xb4"}, {"pu", "\xe3\x81\xb7"}, {"pe", "\xe3\x81\xba"}, {"po", "\xe3\x81\xbd"},
    {"a",  "\xe3\x81\x82"}, {"i",  "\xe3\x81\x84"}, {"u",  "\xe3\x81\x86"}, {"e",  "\xe3\x81\x88"}, {"o",  "\xe3\x81\x8a"},
};

static size_t ja_convert(const char *in, size_t len, char *out, size_t out_cap) {
    size_t out_pos = 0;
    size_t pos = 0;
    while (pos < len) {
        const ja_entry_t *best = NULL;
        size_t best_len = 0;
        for (size_t i = 0; i < sizeof(ja_table) / sizeof(ja_table[0]); i++) {
            size_t rlen = strlen(ja_table[i].romaji);
            if (rlen > best_len && pos + rlen <= len &&
                memcmp(in + pos, ja_table[i].romaji, rlen) == 0) {
                best = &ja_table[i];
                best_len = rlen;
            }
        }
        if (!best) {
            /* Unmatched char: copy as-is */
            if (out_pos + 1 >= out_cap) break;
            out[out_pos++] = in[pos++];
            continue;
        }
        size_t hlen = strlen(best->hiragana);
        if (out_pos + hlen >= out_cap) break;
        memcpy(out + out_pos, best->hiragana, hlen);
        out_pos += hlen;
        pos += best_len;
    }
    out[out_pos] = '\0';
    return out_pos;
}

/* -------------------------------------------------------------------------- */
/* Chinese pinyin → hanzi lookup                                              */
/* -------------------------------------------------------------------------- */
typedef struct {
    const char *pinyin;
    const char *hanzi; /* UTF-8, may contain multiple candidates */
} zh_entry_t;

static const zh_entry_t zh_table[] = {
    {"ni",   "\xe4\xbd\xa0"},     /* 你 */
    {"hao",  "\xe5\xa5\xbd"},     /* 好 */
    {"wo",   "\xe6\x88\x91"},     /* 我 */
    {"shi",  "\xe6\x98\xaf"},     /* 是 */
    {"zai",  "\xe5\x9c\xa8"},     /* 在 */
    {"you",  "\xe6\x9c\x89"},     /* 有 */
    {"ge",   "\xe4\xb8\xaa"},     /* 个 */
    {"ren",  "\xe4\xba\xba"},     /* 人 */
    {"men",  "\xe4\xbb\xac"},     /* 们 */
    {"ta",   "\xe4\xbb\x96"},     /* 他 */
    {"zhong","\xe4\xb8\xad"},     /* 中 */
    {"guo",  "\xe5\x9b\xbd"},     /* 国 */
    {"da",   "\xe5\xa4\xa7"},     /* 大 */
    {"xiao", "\xe5\xb0\x8f"},     /* 小 */
    {"shang","\xe4\xb8\x8a"},     /* 上 */
    {"xia",  "\xe4\xb8\x8b"},     /* 下 */
    {"lai",  "\xe6\x9d\xa5"},     /* 来 */
    {"qu",   "\xe5\x8e\xbb"},     /* 去 */
    {"shuo", "\xe8\xaf\xb4"},     /* 说 */
    {"kan",  "\xe7\x9c\x8b"},     /* 看 */
    {"jian", "\xe8\xa7\x81"},     /* 见 */
    {"xiang","\xe6\x83\xb3"},     /* 想 */
    {"zhi",  "\xe7\x9f\xa5"},     /* 知 */
    {"dao",  "\xe9\x81\x93"},     /* 道 */
    {"neng", "\xe8\x83\xbd"},     /* 能 */
    {"ke",   "\xe5\x8f\xaf"},     /* 可 */
    {"yi",   "\xe4\xbb\xa5"},     /* 以 */
    {"hui",  "\xe4\xbc\x9a"},     /* 会 */
    {"yao",  "\xe8\xa6\x81"},     /* 要 */
    {"jiu",  "\xe5\xb0\xb1"},     /* 就 */
    {"bu",   "\xe4\xb8\x8d"},     /* 不 */
    {"mei",  "\xe6\xb2\xa1"},     /* 没 */
    {"le",   "\xe4\xba\x86"},     /* 了 */
    {"de",   "\xe7\x9a\x84"},     /* 的 */
    {"zhe",  "\xe8\xbf\x99"},     /* 这 */
    {"na",   "\xe9\x82\xa3"},     /* 那 */
    {"he",   "\xe5\x92\x8c"},     /* 和 */
    {"yu",   "\xe4\xb8\x8e"},     /* 与 */
    {"yi1",  "\xe4\xb8\x80"},     /* 一 */
    {"er",   "\xe4\xba\x8c"},     /* 二 */
    {"san",  "\xe4\xb8\x89"},     /* 三 */
    {"si",   "\xe5\x9b\x9b"},     /* 四 */
    {"wu",   "\xe4\xba\x94"},     /* 五 */
    {"liu",  "\xe5\x85\xad"},     /* 六 */
    {"qi",   "\xe4\xb8\x83"},     /* 七 */
    {"ba",   "\xe5\x85\xab"},     /* 八 */
    {"jiu2", "\xe4\xb9\x9d"},     /* 九 */
    {"shi2", "\xe5\x8d\x81"},     /* 十 */
    {"tian", "\xe5\xa4\xa9"},     /* 天 */
    {"di",   "\xe5\x9c\xb0"},     /* 地 */
    {"shui2","\xe6\xb0\xb4"},     /* 水 */
    {"huo",  "\xe7\x81\xab"},     /* 火 */
};

static const char *zh_lookup(const char *pinyin, size_t len) {
    for (size_t i = 0; i < sizeof(zh_table) / sizeof(zh_table[0]); i++) {
        if (strlen(zh_table[i].pinyin) == len &&
            memcmp(zh_table[i].pinyin, pinyin, len) == 0) {
            return zh_table[i].hanzi;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Korean jamo composition                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *ascii;
    uint32_t cp; /* Unicode codepoint */
} jamo_entry_t;

static const jamo_entry_t ko_initial[] = {
    {"g", 0x3131}, {"gg",0x3132}, {"n", 0x3134}, {"d", 0x3137}, {"dd",0x3138},
    {"r", 0x3139}, {"m", 0x3141}, {"b", 0x3142}, {"bb",0x3143}, {"s", 0x3145},
    {"ss",0x3146}, {"o", 0x3147}, {"j", 0x3148}, {"jj",0x3149}, {"ch",0x314A},
    {"k", 0x314B}, {"t", 0x314C}, {"p", 0x314D}, {"h", 0x314E},
};

static const jamo_entry_t ko_vowel[] = {
    {"a",  0x314F}, {"ae", 0x3150}, {"ya", 0x3151}, {"yae",0x3152},
    {"eo", 0x3153}, {"e",  0x3154}, {"yeo",0x3155}, {"ye", 0x3156},
    {"o",  0x3157}, {"wa", 0x3158}, {"wae",0x3159}, {"oe", 0x315A},
    {"yo", 0x315B}, {"u",  0x315C}, {"wo", 0x315D}, {"we", 0x315E},
    {"wi", 0x315F}, {"yu", 0x3160}, {"eu", 0x3161}, {"ui", 0x3162}, {"i", 0x3163},
};

static uint32_t ko_find(const jamo_entry_t *table, size_t n, const char *in, size_t len) {
    for (size_t i = 0; i < n; i++) {
        size_t alen = strlen(table[i].ascii);
        if (alen == len && memcmp(table[i].ascii, in, len) == 0)
            return table[i].cp;
    }
    return 0;
}

/* Compose initial+vowel into a precomposed syllable (no final) */
static uint32_t ko_compose(uint32_t initial_cp, uint32_t vowel_cp) {
    int ini = -1, vow = -1;
    /* Map initial jamo to index */
    switch (initial_cp) {
    case 0x3131: ini = 0; break;
    case 0x3132: ini = 1; break;
    case 0x3134: ini = 2; break;
    case 0x3137: ini = 3; break;
    case 0x3138: ini = 4; break;
    case 0x3139: ini = 5; break;
    case 0x3141: ini = 6; break;
    case 0x3142: ini = 7; break;
    case 0x3143: ini = 8; break;
    case 0x3145: ini = 9; break;
    case 0x3146: ini = 10; break;
    case 0x3147: ini = 11; break;
    case 0x3148: ini = 12; break;
    case 0x3149: ini = 13; break;
    case 0x314A: ini = 14; break;
    case 0x314B: ini = 15; break;
    case 0x314C: ini = 16; break;
    case 0x314D: ini = 17; break;
    case 0x314E: ini = 18; break;
    default: return 0;
    }
    /* Map vowel jamo to index */
    switch (vowel_cp) {
    case 0x314F: vow = 0; break;
    case 0x3150: vow = 1; break;
    case 0x3151: vow = 2; break;
    case 0x3152: vow = 3; break;
    case 0x3153: vow = 4; break;
    case 0x3154: vow = 5; break;
    case 0x3155: vow = 6; break;
    case 0x3156: vow = 7; break;
    case 0x3157: vow = 8; break;
    case 0x3158: vow = 9; break;
    case 0x3159: vow = 10; break;
    case 0x315A: vow = 11; break;
    case 0x315B: vow = 12; break;
    case 0x315C: vow = 13; break;
    case 0x315D: vow = 14; break;
    case 0x315E: vow = 15; break;
    case 0x315F: vow = 16; break;
    case 0x3160: vow = 17; break;
    case 0x3161: vow = 18; break;
    case 0x3162: vow = 19; break;
    case 0x3163: vow = 20; break;
    default: return 0;
    }
    if (ini < 0 || vow < 0) return 0;
    return 0xAC00 + (uint32_t)(ini * 21 * 28 + vow * 28);
}

static size_t ko_convert(const char *in, size_t len, char *out, size_t out_cap) {
    size_t out_pos = 0;
    size_t pos = 0;
    while (pos < len) {
        /* Try longest initial match */
        uint32_t icp = 0;
        size_t ilen = 0;
        for (int l = 3; l >= 1 && !icp; l--) {
            if (pos + (size_t)l <= len) {
                uint32_t c = ko_find(ko_initial, sizeof(ko_initial)/sizeof(ko_initial[0]), in + pos, (size_t)l);
                if (c) { icp = c; ilen = (size_t)l; }
            }
        }
        if (!icp) {
            if (out_pos + 1 >= out_cap) break;
            out[out_pos++] = in[pos++];
            continue;
        }
        /* Try longest vowel match */
        uint32_t vcp = 0;
        size_t vlen = 0;
        for (int l = 3; l >= 1 && !vcp; l--) {
            if (pos + ilen + (size_t)l <= len) {
                uint32_t c = ko_find(ko_vowel, sizeof(ko_vowel)/sizeof(ko_vowel[0]), in + pos + ilen, (size_t)l);
                if (c) { vcp = c; vlen = (size_t)l; }
            }
        }
        if (vcp) {
            uint32_t syl = ko_compose(icp, vcp);
            if (syl) {
                char tmp[4];
                size_t n = utf8_encode(syl, tmp);
                if (out_pos + n < out_cap) {
                    memcpy(out + out_pos, tmp, n);
                    out_pos += n;
                }
                pos += ilen + vlen;
                continue;
            }
        }
        /* Emit initial jamo alone */
        char tmp[4];
        size_t n = utf8_encode(icp, tmp);
        if (out_pos + n < out_cap) {
            memcpy(out + out_pos, tmp, n);
            out_pos += n;
        }
        pos += ilen;
    }
    out[out_pos] = '\0';
    return out_pos;
}

/* -------------------------------------------------------------------------- */
/* Candidate generation                                                       */
/* -------------------------------------------------------------------------- */
static void ime_generate_candidates(void) {
    g_ime.candidate_count = 0;
    g_ime.selected = 0;
    if (g_ime.preedit_len == 0) return;

    /* Try Japanese first */
    char ja_buf[IME_CANDIDATE_LEN];
    size_t ja_len = ja_convert(g_ime.preedit, g_ime.preedit_len, ja_buf, sizeof(ja_buf));
    if (ja_len > 0 && strcmp(ja_buf, g_ime.preedit) != 0) {
        if (g_ime.candidate_count < IME_CANDIDATE_CAP) {
            snprintf(g_ime.candidates[g_ime.candidate_count], IME_CANDIDATE_LEN, "%s", ja_buf);
            g_ime.candidates[g_ime.candidate_count][IME_CANDIDATE_LEN - 1] = '\0';
            g_ime.candidate_count++;
        }
    }

    /* Try Chinese */
    const char *zh = zh_lookup(g_ime.preedit, g_ime.preedit_len);
    if (zh) {
        if (g_ime.candidate_count < IME_CANDIDATE_CAP) {
            snprintf(g_ime.candidates[g_ime.candidate_count], IME_CANDIDATE_LEN, "%s", zh);
            g_ime.candidates[g_ime.candidate_count][IME_CANDIDATE_LEN - 1] = '\0';
            g_ime.candidate_count++;
        }
    }

    /* Try Korean */
    char ko_buf[IME_CANDIDATE_LEN];
    size_t ko_len = ko_convert(g_ime.preedit, g_ime.preedit_len, ko_buf, sizeof(ko_buf));
    if (ko_len > 0 && strcmp(ko_buf, g_ime.preedit) != 0) {
        if (g_ime.candidate_count < IME_CANDIDATE_CAP) {
            snprintf(g_ime.candidates[g_ime.candidate_count], IME_CANDIDATE_LEN, "%s", ko_buf);
            g_ime.candidates[g_ime.candidate_count][IME_CANDIDATE_LEN - 1] = '\0';
            g_ime.candidate_count++;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void ui_ime_init(void) {
    memset(&g_ime, 0, sizeof(g_ime));
    g_ime.active = true;
}

void ui_ime_shutdown(void) {
    memset(&g_ime, 0, sizeof(g_ime));
}

bool ui_ime_process_key(ui_key_t key, uint32_t codepoint, bool ctrl, bool alt, bool shift, bool meta) {
    (void)shift;
    (void)meta;
    if (!g_ime.active) return false;
    if (ctrl || alt) return false;

    /* Number keys select candidates */
    if (key == UI_KEY_NONE && codepoint >= '1' && codepoint <= '9') {
        int idx = (int)(codepoint - '1');
        if (idx < g_ime.candidate_count) {
            g_ime.selected = idx;
            return true;
        }
        return false;
    }

    if (key == UI_KEY_BACKSPACE) {
        if (g_ime.preedit_len > 0) {
            g_ime.preedit_len--;
            g_ime.preedit[g_ime.preedit_len] = '\0';
            ime_generate_candidates();
        }
        return true;
    }

    if (key == UI_KEY_SPACE || key == UI_KEY_ENTER) {
        if (g_ime.preedit_len == 0) return false;
        if (g_ime.candidate_count > 0 && g_ime.selected < g_ime.candidate_count) {
            size_t clen = strlen(g_ime.candidates[g_ime.selected]);
            if (clen < IME_COMMIT_CAP - 1) {
                memcpy(g_ime.committed, g_ime.candidates[g_ime.selected], clen);
                g_ime.committed[clen] = '\0';
                g_ime.committed_len = clen;
            }
        } else {
            /* No valid conversion: commit raw ASCII */
            if (g_ime.preedit_len < IME_COMMIT_CAP - 1) {
                memcpy(g_ime.committed, g_ime.preedit, g_ime.preedit_len);
                g_ime.committed[g_ime.preedit_len] = '\0';
                g_ime.committed_len = g_ime.preedit_len;
            }
        }
        g_ime.preedit_len = 0;
        g_ime.preedit[0] = '\0';
        g_ime.candidate_count = 0;
        g_ime.selected = 0;
        return true;
    }

    if (key == UI_KEY_ESCAPE) {
        ui_ime_reset();
        return true;
    }

    /* ASCII letters */
    if (key == UI_KEY_NONE && codepoint >= 'a' && codepoint <= 'z') {
        if (g_ime.preedit_len + 1 < IME_PREEDIT_CAP) {
            g_ime.preedit[g_ime.preedit_len++] = (char)codepoint;
            g_ime.preedit[g_ime.preedit_len] = '\0';
            ime_generate_candidates();
        }
        return true;
    }

    return false;
}

const char *ui_ime_get_preedit(void) {
    return g_ime.preedit;
}

const char *ui_ime_commit(void) {
    if (g_ime.committed_len == 0) return NULL;
    g_ime.committed_len = 0;
    return g_ime.committed;
}

int ui_ime_candidate_count(void) {
    return g_ime.candidate_count;
}

const char *ui_ime_candidate(int index) {
    if (index < 0 || index >= g_ime.candidate_count) return NULL;
    return g_ime.candidates[index];
}

void ui_ime_select_candidate(int index) {
    if (index >= 0 && index < g_ime.candidate_count)
        g_ime.selected = index;
}

void ui_ime_reset(void) {
    g_ime.preedit_len = 0;
    g_ime.preedit[0] = '\0';
    g_ime.committed_len = 0;
    g_ime.committed[0] = '\0';
    g_ime.candidate_count = 0;
    g_ime.selected = 0;
}
