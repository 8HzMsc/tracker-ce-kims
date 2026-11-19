// PTFX_NAME: Say Speech Gen
// PTFX_AUTHOR: Polyend CE
// PTFX_FLAGS: GENERATOR
// PTFX_SCRATCH: 16384
//
// Clean-room MIT formant speech synthesizer (Amiga Say homage).
// Uses public-domain NRL Report 7948 (1976) English letter-to-phoneme rules
// paired with a Klatt-style cascade/parallel formant vocal tract model.

#include "tracker_plugin.h"

const PluginParam PLUGIN_PARAMS[8] = {
    PTFX_PARAM_TEXT("Text", "HELLO TRACKER"),
    PTFX_PARAM_LIST("Voice", 0, "Male|Female"),
    PTFX_PARAM_LIST("Mode", 0, "Natural|Robot|Whisper"),
    PTFX_PARAM_INT("Pitch", 65.0f, 320.0f, 110.0f, 1.0f, " Hz"),
    PTFX_PARAM_INT("Speed", 40.0f, 400.0f, 150.0f, 5.0f, " WPM"),
    PTFX_PARAM_INT("Mouth", 50.0f, 200.0f, 100.0f, 2.0f, " %"),
    PTFX_PARAM_INT("Throat", 50.0f, 200.0f, 100.0f, 2.0f, " %"),
    PTFX_PARAM_LIST("Grime", 1, "Clean|Amiga 8b|Lo-Fi")
};

#define SAY_INTERNAL_SR 22050.0f
#define SAY_MAX_TEXT    64
#define SAY_MAX_PHONES  256
#define SAY_MAX_SECONDS 10
#define SAY_MAX_SAMPLES (44100u * SAY_MAX_SECONDS)
#define SAY_PI          3.14159265f

// Phoneme IDs
enum {
    PH_SIL = 0,
    // Vowels
    PH_IY, PH_IH, PH_EY, PH_EH, PH_AE, PH_AA, PH_AO, PH_OW, PH_UH, PH_UW, PH_AH, PH_AX, PH_ER,
    // Diphthongs (expanded into 2 vowel frames during queueing)
    PH_AY, PH_AW, PH_OY,
    // Liquids, glides, nasals
    PH_L, PH_R, PH_W, PH_Y, PH_M, PH_N, PH_NG,
    // Fricatives
    PH_F, PH_V, PH_TH, PH_DH, PH_S, PH_Z, PH_SH, PH_ZH, PH_HH,
    // Plosives (stops: closure + burst)
    PH_P, PH_B, PH_T, PH_D, PH_K, PH_G,
    // Affricates (expanded to T+SH / D+ZH)
    PH_CH, PH_JH,
    // Pauses
    PH_PAU_WORD, PH_PAU_COMMA, PH_PAU_PERIOD,
    PH_COUNT
};

// Phoneme flags
#define PF_VOICED    0x01u
#define PF_VOWEL     0x02u
#define PF_FRIC      0x04u
#define PF_STOP      0x08u
#define PF_NASAL     0x10u
#define PF_ASPIRATED 0x20u

typedef struct {
    uint16_t f1;       // Formant 1 Hz
    uint16_t f2;       // Formant 2 Hz
    uint16_t f3;       // Formant 3 Hz
    uint16_t bw1;      // Bandwidth 1 Hz
    uint16_t bw2;      // Bandwidth 2 Hz
    uint16_t bw3;      // Bandwidth 3 Hz
    uint16_t fric_f;   // Parallel fricative/burst center Hz
    uint16_t fric_bw;  // Parallel fricative/burst bandwidth Hz
    uint8_t  dur_ms;   // Nominal duration at 150 WPM (ms)
    uint8_t  v_gain;   // Voiced gain (0..100)
    uint8_t  n_gain;   // Noise/fricative gain (0..100)
    uint8_t  flags;    // PF_* bitmask
} PhoneDef;

// Textbook Klatt / Peterson-Barney formant table (male baseline, 22.05 kHz safe)
static const PhoneDef kPhoneTable[PH_COUNT] = {
    //               f1    f2    f3   bw1  bw2  bw3  frF  frBW dur  vG  nG  flags
    [PH_SIL]     = { 500, 1500, 2500, 100, 150, 200, 3000, 800,  40,  0,  0, 0 },
    // Vowels
    [PH_IY]      = { 280, 2250, 2890,  60,  90, 150, 3000, 800, 115, 95,  0, PF_VOICED | PF_VOWEL },
    [PH_IH]      = { 400, 1920, 2560,  65, 100, 150, 3000, 800,  85, 95,  0, PF_VOICED | PF_VOWEL },
    [PH_EY]      = { 480, 2080, 2650,  70, 100, 150, 3000, 800, 125, 95,  0, PF_VOICED | PF_VOWEL },
    [PH_EH]      = { 550, 1770, 2490,  75, 100, 160, 3000, 800,  95, 98,  0, PF_VOICED | PF_VOWEL },
    [PH_AE]      = { 690, 1660, 2490,  85, 110, 170, 3000, 800, 120, 100, 0, PF_VOICED | PF_VOWEL },
    [PH_AA]      = { 710, 1100, 2540,  85, 110, 170, 3000, 800, 120, 100, 0, PF_VOICED | PF_VOWEL },
    [PH_AO]      = { 590,  880, 2540,  80, 100, 160, 3000, 800, 115, 98,  0, PF_VOICED | PF_VOWEL },
    [PH_OW]      = { 460,  910, 2450,  75,  95, 160, 3000, 800, 125, 95,  0, PF_VOICED | PF_VOWEL },
    [PH_UH]      = { 450, 1030, 2380,  70, 100, 160, 3000, 800,  85, 92,  0, PF_VOICED | PF_VOWEL },
    [PH_UW]      = { 310,  870, 2250,  65,  95, 150, 3000, 800, 115, 90,  0, PF_VOICED | PF_VOWEL },
    [PH_AH]      = { 620, 1220, 2550,  80, 105, 160, 3000, 800,  90, 96,  0, PF_VOICED | PF_VOWEL },
    [PH_AX]      = { 500, 1400, 2480,  80, 110, 170, 3000, 800,  65, 85,  0, PF_VOICED | PF_VOWEL },
    [PH_ER]      = { 480, 1350, 1690,  75, 100, 140, 3000, 800, 110, 94,  0, PF_VOICED | PF_VOWEL },
    // Diphthongs (placeholders; expanded into vowel pairs)
    [PH_AY]      = { 710, 1100, 2540,  85, 110, 170, 3000, 800, 130, 98,  0, PF_VOICED | PF_VOWEL },
    [PH_AW]      = { 710, 1100, 2540,  85, 110, 170, 3000, 800, 130, 98,  0, PF_VOICED | PF_VOWEL },
    [PH_OY]      = { 590,  880, 2540,  80, 100, 160, 3000, 800, 130, 98,  0, PF_VOICED | PF_VOWEL },
    // Liquids, glides, nasals
    [PH_L]       = { 360, 1050, 2650,  70, 110, 180, 3000, 800,  75, 80,  0, PF_VOICED },
    [PH_R]       = { 420, 1280, 1620,  75, 105, 140, 3000, 800,  75, 82,  0, PF_VOICED },
    [PH_W]       = { 300,  680, 2250,  65,  90, 160, 3000, 800,  75, 82,  0, PF_VOICED },
    [PH_Y]       = { 270, 2200, 2950,  60,  95, 160, 3000, 800,  70, 82,  0, PF_VOICED },
    [PH_M]       = { 280, 1050, 2200,  90, 180, 240, 2000, 900,  78, 68,  0, PF_VOICED | PF_NASAL },
    [PH_N]       = { 280, 1650, 2500,  90, 180, 240, 2500, 900,  75, 68,  0, PF_VOICED | PF_NASAL },
    [PH_NG]      = { 280, 2100, 2600,  90, 180, 240, 2500, 900,  82, 65,  0, PF_VOICED | PF_NASAL },
    // Fricatives
    [PH_F]       = { 400, 1300, 2400, 150, 200, 250, 4400, 2200, 85,  0, 48, PF_FRIC },
    [PH_V]       = { 320, 1300, 2400, 120, 180, 240, 4000, 2000, 75, 55, 35, PF_VOICED | PF_FRIC },
    [PH_TH]      = { 400, 1500, 2500, 150, 200, 250, 4800, 2400, 85,  0, 42, PF_FRIC },
    [PH_DH]      = { 320, 1500, 2500, 120, 180, 240, 4200, 2200, 68, 58, 30, PF_VOICED | PF_FRIC },
    [PH_S]       = { 400, 1750, 2650, 150, 200, 250, 5600, 1400, 95,  0, 78, PF_FRIC },
    [PH_Z]       = { 320, 1750, 2650, 120, 180, 240, 5200, 1500, 82, 55, 58, PF_VOICED | PF_FRIC },
    [PH_SH]      = { 400, 1950, 2600, 150, 200, 250, 2850,  900, 95,  0, 76, PF_FRIC },
    [PH_ZH]      = { 320, 1950, 2600, 120, 180, 240, 2750,  950, 80, 55, 55, PF_VOICED | PF_FRIC },
    [PH_HH]      = { 500, 1500, 2500, 120, 160, 220, 1800, 1400, 65,  0, 45, PF_ASPIRATED },
    // Stops (closure + release burst handled in synth state)
    [PH_P]       = { 300,  950, 2150, 120, 180, 240, 1100,  900, 78,  0, 62, PF_STOP },
    [PH_B]       = { 240,  950, 2150, 100, 160, 220, 1000,  900, 68, 42, 32, PF_VOICED | PF_STOP },
    [PH_T]       = { 320, 1750, 2650, 120, 180, 240, 4100, 1500, 74,  0, 72, PF_STOP },
    [PH_D]       = { 260, 1750, 2650, 100, 160, 220, 3600, 1500, 66, 45, 36, PF_VOICED | PF_STOP },
    [PH_K]       = { 320, 1900, 2400, 120, 180, 240, 2200, 1000, 80,  0, 68, PF_STOP },
    [PH_G]       = { 260, 1900, 2400, 100, 160, 220, 2000, 1000, 70, 45, 34, PF_VOICED | PF_STOP },
    // Affricates (expanded)
    [PH_CH]      = { 400, 1950, 2600, 150, 200, 250, 2900,  900, 95,  0, 76, PF_FRIC },
    [PH_JH]      = { 320, 1950, 2600, 120, 180, 240, 2800,  950, 85, 55, 55, PF_VOICED | PF_FRIC },
    // Pauses
    [PH_PAU_WORD]   = { 500, 1500, 2500, 100, 150, 200, 3000, 800,  50, 0, 0, 0 },
    [PH_PAU_COMMA]  = { 500, 1500, 2500, 100, 150, 200, 3000, 800, 160, 0, 0, 0 },
    [PH_PAU_PERIOD] = { 500, 1500, 2500, 100, 150, 200, 3000, 800, 240, 0, 0, 0 },
};

// ============================================================================
// Compact NRL-style English Letter-to-Phoneme Rules (Public Domain 1976 rules)
// Stored with fixed inline arrays (zero pointer relocations for PIC .ptfx!)
// Pattern symbols:
//   '#' = 1 or more vowels (A,E,I,O,U,Y)
//   ':' = 0 or more consonants
//   '^' = 1 consonant
//   '.' = voiced consonant (B,D,V,G,J,L,M,N,R,W,Z)
//   '+' = front vowel (E,I,Y)
//   ' ' = word boundary (non-letter)
// ============================================================================
typedef struct {
    char left[5];
    char mid[7];
    char right[6];
    uint8_t out[4]; // up to 4 phonemes, 0 (PH_SIL) = end
} LtsRule;

static const LtsRule kLtsRules[] = {
    // High-frequency irregular words (exact word-bounded matches)
    { " ", "THE",     " ",  { PH_DH, PH_AX, 0, 0 } },
    { " ", "YOU",     " ",  { PH_Y,  PH_UW, 0, 0 } },
    { " ", "YOUR",    " ",  { PH_Y,  PH_AO, PH_R, 0 } },
    { " ", "ARE",     " ",  { PH_AA, PH_R,  0, 0 } },
    { " ", "ONE",     " ",  { PH_W,  PH_AH, PH_N, 0 } },
    { " ", "TWO",     " ",  { PH_T,  PH_UW, 0, 0 } },
    { " ", "FOUR",    " ",  { PH_F,  PH_AO, PH_R, 0 } },
    { " ", "EIGHT",   " ",  { PH_EY, PH_T,  0, 0 } },
    { " ", "TO",      " ",  { PH_T,  PH_UW, 0, 0 } },
    { " ", "DO",      " ",  { PH_D,  PH_UW, 0, 0 } },
    { " ", "OF",      " ",  { PH_AH, PH_V,  0, 0 } },
    { " ", "HAVE",    " ",  { PH_HH, PH_AE, PH_V, 0 } },
    { " ", "WHAT",    " ",  { PH_W,  PH_AH, PH_T, 0 } },
    { " ", "WHO",     " ",  { PH_HH, PH_UW, 0, 0 } },
    { " ", "WHERE",   " ",  { PH_W,  PH_EH, PH_R, 0 } },
    { " ", "THERE",   " ",  { PH_DH, PH_EH, PH_R, 0 } },
    { " ", "HERE",    " ",  { PH_HH, PH_IH, PH_R, 0 } },
    { " ", "THIS",    " ",  { PH_DH, PH_IH, PH_S, 0 } },
    { " ", "THAT",    " ",  { PH_DH, PH_AE, PH_T, 0 } },
    { " ", "THEY",    " ",  { PH_DH, PH_EY, 0, 0 } },
    { " ", "THEM",    " ",  { PH_DH, PH_EH, PH_M, 0 } },
    { " ", "WITH",    " ",  { PH_W,  PH_IH, PH_DH, 0 } },
    { " ", "FROM",    " ",  { PH_F,  PH_R,  PH_AH, PH_M } },
    { " ", "SOME",    " ",  { PH_S,  PH_AH, PH_M, 0 } },
    { " ", "COME",    " ",  { PH_K,  PH_AH, PH_M, 0 } },
    { " ", "LOVE",    " ",  { PH_L,  PH_AH, PH_V, 0 } },
    { " ", "HELLO",   " ",  { PH_HH, PH_EH, PH_L, PH_OW } },
    { " ", "WORLD",   " ",  { PH_W,  PH_ER, PH_L, PH_D } },
    { " ", "SYNTH",   " ",  { PH_S,  PH_IH, PH_N, PH_TH } },
    { " ", "POLY",    "",   { PH_P,  PH_AA, PH_L, PH_IY } },
    { " ", "SAY",     " ",  { PH_S,  PH_EY, 0, 0 } },
    { " ", "I",       " ",  { PH_AY, 0,     0, 0 } },
    { " ", "A",       " ",  { PH_AX, 0,     0, 0 } },

    // Common suffixes & multi-letter clusters
    { "",  "TION",    "",   { PH_SH, PH_AX, PH_N, 0 } },
    { "",  "SION",    "",   { PH_ZH, PH_AX, PH_N, 0 } },
    { "",  "OUGH",    "",   { PH_OW, 0,     0, 0 } },
    { "",  "IGHT",    "",   { PH_AY, PH_T,  0, 0 } },
    { "",  "AUGH",    "",   { PH_AO, 0,     0, 0 } },
    { "",  "EIGH",    "",   { PH_EY, 0,     0, 0 } },
    { "",  "ING",     " ",  { PH_IH, PH_NG, 0, 0 } },
    { "",  "ABLE",    " ",  { PH_AX, PH_B,  PH_AX, PH_L } },
    { "",  "FULL",    "",   { PH_F,  PH_UH, PH_L, 0 } },
    { "",  "NESS",    " ",  { PH_N,  PH_AX, PH_S, 0 } },
    { "",  "MENT",    " ",  { PH_M,  PH_AX, PH_N, PH_T } },
    { "",  "SCH",     "",   { PH_S,  PH_K,  0, 0 } },
    { "",  "TCH",     "",   { PH_CH, 0,     0, 0 } },
    { "",  "DGE",     "",   { PH_JH, 0,     0, 0 } },
    { "",  "CK",      "",   { PH_K,  0,     0, 0 } },
    { "",  "CH",      "",   { PH_CH, 0,     0, 0 } },
    { "",  "SH",      "",   { PH_SH, 0,     0, 0 } },
    { "",  "PH",      "",   { PH_F,  0,     0, 0 } },
    { "",  "WH",      "",   { PH_W,  0,     0, 0 } },
    { "",  "TH",      "E ", { PH_DH, 0,     0, 0 } },
    { "",  "TH",      "",   { PH_TH, 0,     0, 0 } },
    { "",  "NG",      "",   { PH_NG, 0,     0, 0 } },
    { "",  "NK",      "",   { PH_NG, PH_K,  0, 0 } },
    { "",  "QU",      "",   { PH_K,  PH_W,  0, 0 } },
    { " ", "KN",      "",   { PH_N,  0,     0, 0 } },
    { " ", "WR",      "",   { PH_R,  0,     0, 0 } },
    { "",  "GH",      "",   { 0,     0,     0, 0 } }, // silent GH
    { "",  "MB",      " ",  { PH_M,  0,     0, 0 } },
    { "",  "LE",      " ",  { PH_AX, PH_L,  0, 0 } },

    // A rules
    { "",  "AI",      "",   { PH_EY, 0, 0, 0 } },
    { "",  "AY",      "",   { PH_EY, 0, 0, 0 } },
    { "",  "AU",      "",   { PH_AO, 0, 0, 0 } },
    { "",  "AW",      "",   { PH_AO, 0, 0, 0 } },
    { "",  "ALL",     "",   { PH_AO, PH_L, 0, 0 } },
    { "",  "ALK",     "",   { PH_AO, PH_K, 0, 0 } },
    { "",  "AR",      " ",  { PH_AA, PH_R, 0, 0 } },
    { "",  "AR",      "^",  { PH_AA, PH_R, 0, 0 } },
    { "",  "ARE",     " ",  { PH_EH, PH_R, 0, 0 } },
    { "",  "AIR",     "",   { PH_EH, PH_R, 0, 0 } },
    { "",  "A",       "^E ",{ PH_EY, 0, 0, 0 } },
    { "",  "A",       "^+", { PH_EY, 0, 0, 0 } },
    { "#", "A",       " ",  { PH_AX, 0, 0, 0 } },
    { "",  "A",       "",   { PH_AE, 0, 0, 0 } },

    // B rules
    { "",  "BB",      "",   { PH_B,  0, 0, 0 } },
    { "",  "B",       "",   { PH_B,  0, 0, 0 } },

    // C rules
    { "",  "CI",      "#",  { PH_SH, 0, 0, 0 } },
    { "",  "C",       "+",  { PH_S,  0, 0, 0 } },
    { "",  "CC",      "+",  { PH_K,  PH_S, 0, 0 } },
    { "",  "CC",      "",   { PH_K,  0, 0, 0 } },
    { "",  "C",       "",   { PH_K,  0, 0, 0 } },

    // D rules
    { "",  "DD",      "",   { PH_D,  0, 0, 0 } },
    { "",  "D",       "",   { PH_D,  0, 0, 0 } },

    // E rules
    { "",  "EE",      "",   { PH_IY, 0, 0, 0 } },
    { "",  "EA",      "R",  { PH_IH, 0, 0, 0 } },
    { "",  "EA",      "",   { PH_IY, 0, 0, 0 } },
    { "",  "EI",      "",   { PH_IY, 0, 0, 0 } },
    { "",  "EY",      " ",  { PH_IY, 0, 0, 0 } },
    { "",  "EY",      "",   { PH_EY, 0, 0, 0 } },
    { "",  "EW",      "",   { PH_Y,  PH_UW, 0, 0 } },
    { "",  "EU",      "",   { PH_Y,  PH_UW, 0, 0 } },
    { "",  "ER",      "",   { PH_ER, 0, 0, 0 } },
    { "#", "ED",      " ",  { PH_D,  0, 0, 0 } },
    { "#", "ES",      " ",  { PH_Z,  0, 0, 0 } },
    { "#:","E",       " ",  { 0,     0, 0, 0 } }, // silent final E
    { "",  "E",       " ",  { PH_IY, 0, 0, 0 } }, // short word final E (ME, BE, WE)
    { "",  "E",       "^E ",{ PH_IY, 0, 0, 0 } },
    { "",  "E",       "",   { PH_EH, 0, 0, 0 } },

    // F rules
    { "",  "FF",      "",   { PH_F,  0, 0, 0 } },
    { "",  "F",       "",   { PH_F,  0, 0, 0 } },

    // G rules
    { "",  "GG",      "",   { PH_G,  0, 0, 0 } },
    { "",  "GE",      " ",  { PH_JH, 0, 0, 0 } },
    { "",  "G",       "+",  { PH_JH, 0, 0, 0 } },
    { "",  "G",       "",   { PH_G,  0, 0, 0 } },

    // H rules
    { "",  "H",       "#",  { PH_HH, 0, 0, 0 } },
    { "",  "H",       "",   { 0,     0, 0, 0 } }, // silent H before consonant/end

    // I rules
    { "",  "IE",      " ",  { PH_IY, 0, 0, 0 } },
    { "",  "IE",      "",   { PH_IY, 0, 0, 0 } },
    { "",  "IR",      "^",  { PH_ER, 0, 0, 0 } },
    { "",  "IR",      " ",  { PH_ER, 0, 0, 0 } },
    { "",  "I",       "^E ",{ PH_AY, 0, 0, 0 } },
    { "",  "I",       "ND", { PH_AY, 0, 0, 0 } },
    { "",  "I",       "LD", { PH_AY, 0, 0, 0 } },
    { "",  "I",       "",   { PH_IH, 0, 0, 0 } },

    // J, K, L, M, N rules
    { "",  "J",       "",   { PH_JH, 0, 0, 0 } },
    { "",  "K",       "",   { PH_K,  0, 0, 0 } },
    { "",  "LL",      "",   { PH_L,  0, 0, 0 } },
    { "",  "L",       "",   { PH_L,  0, 0, 0 } },
    { "",  "MM",      "",   { PH_M,  0, 0, 0 } },
    { "",  "M",       "",   { PH_M,  0, 0, 0 } },
    { "",  "NN",      "",   { PH_N,  0, 0, 0 } },
    { "",  "N",       "",   { PH_N,  0, 0, 0 } },

    // O rules
    { "",  "OO",      "K",  { PH_UH, 0, 0, 0 } },
    { "",  "OO",      "D",  { PH_UH, 0, 0, 0 } },
    { "",  "OO",      "",   { PH_UW, 0, 0, 0 } },
    { "",  "OA",      "",   { PH_OW, 0, 0, 0 } },
    { "",  "OI",      "",   { PH_OY, 0, 0, 0 } },
    { "",  "OY",      "",   { PH_OY, 0, 0, 0 } },
    { "",  "OU",      "",   { PH_AW, 0, 0, 0 } },
    { "",  "OW",      " ",  { PH_OW, 0, 0, 0 } },
    { "",  "OW",      "N",  { PH_AW, 0, 0, 0 } },
    { "",  "OW",      "",   { PH_OW, 0, 0, 0 } },
    { "",  "OR",      "",   { PH_AO, PH_R, 0, 0 } },
    { "",  "O",       "LD", { PH_OW, 0, 0, 0 } },
    { "",  "O",       "^E ",{ PH_OW, 0, 0, 0 } },
    { "",  "O",       " ",  { PH_OW, 0, 0, 0 } },
    { "",  "O",       "",   { PH_AA, 0, 0, 0 } },

    // P, Q, R rules
    { "",  "PP",      "",   { PH_P,  0, 0, 0 } },
    { "",  "P",       "",   { PH_P,  0, 0, 0 } },
    { "",  "Q",       "",   { PH_K,  0, 0, 0 } },
    { "",  "RR",      "",   { PH_R,  0, 0, 0 } },
    { "",  "R",       "",   { PH_R,  0, 0, 0 } },

    // S rules
    { "",  "SS",      "",   { PH_S,  0, 0, 0 } },
    { "",  "SH",      "",   { PH_SH, 0, 0, 0 } },
    { ".", "S",       " ",  { PH_Z,  0, 0, 0 } },
    { "#", "S",       "#",  { PH_Z,  0, 0, 0 } },
    { "",  "S",       "",   { PH_S,  0, 0, 0 } },

    // T rules
    { "",  "TT",      "",   { PH_T,  0, 0, 0 } },
    { "",  "T",       "",   { PH_T,  0, 0, 0 } },

    // U rules
    { "",  "UR",      "^",  { PH_ER, 0, 0, 0 } },
    { "",  "UR",      " ",  { PH_ER, 0, 0, 0 } },
    { "",  "UE",      " ",  { PH_UW, 0, 0, 0 } },
    { "",  "UI",      "",   { PH_UW, 0, 0, 0 } },
    { "",  "U",       "^E ",{ PH_Y,  PH_UW, 0, 0 } },
    { "",  "U",       "",   { PH_AH, 0, 0, 0 } },

    // V, W, X, Y, Z rules
    { "",  "V",       "",   { PH_V,  0, 0, 0 } },
    { "",  "W",       "",   { PH_W,  0, 0, 0 } },
    { "",  "X",       "",   { PH_K,  PH_S, 0, 0 } },
    { " ", "Y",       "",   { PH_Y,  0, 0, 0 } },
    { "",  "Y",       "#",  { PH_Y,  0, 0, 0 } },
    { "#", "Y",       " ",  { PH_IY, 0, 0, 0 } },
    { "",  "Y",       " ",  { PH_AY, 0, 0, 0 } },
    { "",  "Y",       "",   { PH_IH, 0, 0, 0 } },
    { "",  "ZZ",      "",   { PH_Z,  0, 0, 0 } },
    { "",  "Z",       "",   { PH_Z,  0, 0, 0 } },
};

#define NUM_LTS_RULES ((uint32_t)(sizeof(kLtsRules) / sizeof(kLtsRules[0])))

// Digit words (inline char arrays, no pointer relocations!)
static const char kDigitWords[10][6] = {
    "ZERO", "ONE", "TWO", "THREE", "FOUR",
    "FIVE", "SIX", "SEVEN", "EIGHT", "NINE"
};

// Raw phoneme token table for [HH EH L OW] mode
typedef struct {
    char name[3];
    uint8_t id;
} PhoneName;

static const PhoneName kPhoneNames[] = {
    {"IY", PH_IY}, {"IH", PH_IH}, {"EY", PH_EY}, {"EH", PH_EH}, {"AE", PH_AE},
    {"AA", PH_AA}, {"AO", PH_AO}, {"OW", PH_OW}, {"UH", PH_UH}, {"UW", PH_UW},
    {"AH", PH_AH}, {"AX", PH_AX}, {"ER", PH_ER}, {"AY", PH_AY}, {"AW", PH_AW},
    {"OY", PH_OY}, {"L",  PH_L},  {"R",  PH_R},  {"W",  PH_W},  {"Y",  PH_Y},
    {"M",  PH_M},  {"N",  PH_N},  {"NG", PH_NG}, {"F",  PH_F},  {"V",  PH_V},
    {"TH", PH_TH}, {"DH", PH_DH}, {"S",  PH_S},  {"Z",  PH_Z},  {"SH", PH_SH},
    {"ZH", PH_ZH}, {"HH", PH_HH}, {"P",  PH_P},  {"B",  PH_B},  {"T",  PH_T},
    {"D",  PH_D},  {"K",  PH_K},  {"G",  PH_G},  {"CH", PH_CH}, {"JH", PH_JH},
};
#define NUM_PHONE_NAMES ((uint32_t)(sizeof(kPhoneNames) / sizeof(kPhoneNames[0])))

// ============================================================================
// Resonator & Synth Runtime State (placed in scratch_mem!)
// ============================================================================
typedef struct {
    float a;
    float b;
    float c;
    float y1;
    float y2;
} Resonator;

typedef struct {
    uint8_t  phone_id;
    uint8_t  stress;
    uint16_t dur_samples; // at 22050 Hz
} QueuedPhone;

typedef struct {
    QueuedPhone queue[SAY_MAX_PHONES];
    uint16_t    queue_len;
    uint16_t    queue_pos;
    uint32_t    phone_sample_idx;
    uint32_t    total_internal_samples;
    uint32_t    rendered_internal_samples;
    uint8_t     is_question;
    uint8_t     prepared;

    // Expanded uppercase text buffer
    char        expanded[192];

    // Current interpolated formant parameters
    float cur_f1, cur_f2, cur_f3;
    float cur_bw1, cur_bw2, cur_bw3;
    float cur_fr_f, cur_fr_bw;
    float cur_v_gain, cur_n_gain;
    float cur_pitch;

    // Resonators
    Resonator r1, r2, r3, r4;
    Resonator r_fric;

    // Glottal & noise oscillator state
    float    glot_phase;
    float    prev_glot;
    float    lp_state;
    float    prev_internal_sample;
    float    curr_internal_sample;
    uint8_t  upsample_phase;
    uint8_t  lofi_hold_count;
    float    lofi_held_sample;
    uint32_t rng_state;
} SayState;

// Static parameters (tiny footprint in .bss)
static const PtfxHostApi* s_host = 0;
static SayState*          s_st   = 0;

static uint8_t s_voice  = 0;      // 0=Male, 1=Female
static uint8_t s_mode   = 0;      // 0=Natural, 1=Robot, 2=Whisper
static float   s_pitch  = 110.0f; // Hz
static float   s_speed  = 150.0f; // WPM
static float   s_mouth  = 100.0f; // %
static float   s_throat = 100.0f; // %
static uint8_t s_grime  = 1;      // 0=Clean, 1=Amiga 8b, 2=Lo-Fi

// ============================================================================
// Freestanding character & matching helpers
// ============================================================================
static inline char to_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}
static inline int is_alpha(char c) {
    return (c >= 'A' && c <= 'Z');
}
static inline int is_vowel(char c) {
    return (c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == 'Y');
}
static inline int is_consonant(char c) {
    return is_alpha(c) && !is_vowel(c);
}
static inline int is_voiced_cons(char c) {
    return (c == 'B' || c == 'D' || c == 'V' || c == 'G' || c == 'J' ||
            c == 'L' || c == 'M' || c == 'N' || c == 'R' || c == 'W' || c == 'Z');
}
static inline int is_front_vowel(char c) {
    return (c == 'E' || c == 'I' || c == 'Y');
}

static uint32_t str_len_max(const char* s, uint32_t max_n) {
    uint32_t n = 0;
    if (!s) return 0;
    while (n < max_n && s[n] != '\0') n++;
    return n;
}

static int match_left(const char* pat, const char* text, int pos) {
    int plen = (int)str_len_max(pat, 5);
    int pi = plen - 1;
    int ti = pos - 1;
    while (pi >= 0) {
        char p = pat[pi];
        if (p == ' ') {
            if (ti >= 0 && is_alpha(text[ti])) return 0;
            pi--;
        } else if (p == '#') {
            if (ti < 0 || !is_vowel(text[ti])) return 0;
            while (ti >= 0 && is_vowel(text[ti])) ti--;
            pi--;
        } else if (p == ':') {
            while (ti >= 0 && is_consonant(text[ti])) ti--;
            pi--;
        } else if (p == '^') {
            if (ti < 0 || !is_consonant(text[ti])) return 0;
            ti--; pi--;
        } else if (p == '.') {
            if (ti < 0 || !is_voiced_cons(text[ti])) return 0;
            ti--; pi--;
        } else if (p == '+') {
            if (ti < 0 || !is_front_vowel(text[ti])) return 0;
            ti--; pi--;
        } else {
            if (ti < 0 || text[ti] != p) return 0;
            ti--; pi--;
        }
    }
    return 1;
}

static int match_right(const char* pat, const char* text, int pos, int tlen) {
    int pi = 0;
    int ti = pos;
    while (pat[pi] != '\0') {
        char p = pat[pi];
        if (p == ' ') {
            if (ti < tlen && is_alpha(text[ti])) return 0;
            pi++;
        } else if (p == '#') {
            if (ti >= tlen || !is_vowel(text[ti])) return 0;
            while (ti < tlen && is_vowel(text[ti])) ti++;
            pi++;
        } else if (p == ':') {
            while (ti < tlen && is_consonant(text[ti])) ti++;
            pi++;
        } else if (p == '^') {
            if (ti >= tlen || !is_consonant(text[ti])) return 0;
            ti++; pi++;
        } else if (p == '.') {
            if (ti >= tlen || !is_voiced_cons(text[ti])) return 0;
            ti++; pi++;
        } else if (p == '+') {
            if (ti >= tlen || !is_front_vowel(text[ti])) return 0;
            ti++; pi++;
        } else {
            if (ti >= tlen || text[ti] != p) return 0;
            ti++; pi++;
        }
    }
    return 1;
}

static void push_phone(SayState* st, uint8_t ph, uint8_t stress) {
    if (ph == PH_SIL || ph >= PH_COUNT) return;
    // Expand diphthongs & affricates into smooth two-part sequences
    if (ph == PH_AY) { push_phone(st, PH_AA, stress); push_phone(st, PH_IY, 0); return; }
    if (ph == PH_AW) { push_phone(st, PH_AA, stress); push_phone(st, PH_UW, 0); return; }
    if (ph == PH_OY) { push_phone(st, PH_AO, stress); push_phone(st, PH_IY, 0); return; }
    if (ph == PH_CH) { push_phone(st, PH_T, 0); push_phone(st, PH_SH, 0); return; }
    if (ph == PH_JH) { push_phone(st, PH_D, 0); push_phone(st, PH_ZH, 0); return; }

    if (st->queue_len >= SAY_MAX_PHONES) return;
    float speed_scale = 150.0f / (s_speed > 20.0f ? s_speed : 150.0f);
    float ms = (float)kPhoneTable[ph].dur_ms * speed_scale;
    if (stress && (kPhoneTable[ph].flags & PF_VOWEL)) {
        ms *= 1.15f;
    }
    uint32_t samps = (uint32_t)(ms * (SAY_INTERNAL_SR / 1000.0f));
    if (samps < 110u) samps = 110u;
    if (samps > 12000u) samps = 12000u;

    st->queue[st->queue_len].phone_id = ph;
    st->queue[st->queue_len].stress = stress;
    st->queue[st->queue_len].dur_samples = (uint16_t)samps;
    st->queue_len++;
    st->total_internal_samples += samps;
}

static void expand_input_text(SayState* st, const char* raw) {
    uint32_t out = 0;
    st->is_question = 0;
    if (!raw || raw[0] == '\0') {
        raw = "HELLO TRACKER";
    }
    for (uint32_t i = 0; raw[i] != '\0' && out < sizeof(st->expanded) - 8; i++) {
        char c = to_upper(raw[i]);
        if (c == '?') st->is_question = 1;
        if (c >= '0' && c <= '9') {
            if (out > 0 && st->expanded[out - 1] != ' ') st->expanded[out++] = ' ';
            const char* w = kDigitWords[c - '0'];
            for (uint32_t k = 0; w[k] != '\0' && out < sizeof(st->expanded) - 2; k++) {
                st->expanded[out++] = w[k];
            }
            st->expanded[out++] = ' ';
        } else if (c == '+') {
            const char w[] = " PLUS ";
            for (uint32_t k = 0; w[k] != '\0' && out < sizeof(st->expanded) - 2; k++)
                st->expanded[out++] = w[k];
        } else if (c == '@') {
            const char w[] = " AT ";
            for (uint32_t k = 0; w[k] != '\0' && out < sizeof(st->expanded) - 2; k++)
                st->expanded[out++] = w[k];
        } else {
            st->expanded[out++] = c;
        }
    }
    st->expanded[out] = '\0';
}

static void text_to_phonemes(SayState* st, const char* raw) {
    st->queue_len = 0;
    st->total_internal_samples = 0;
    expand_input_text(st, raw);

    // Small leading silence
    push_phone(st, PH_PAU_WORD, 0);

    const char* txt = st->expanded;
    int tlen = (int)str_len_max(txt, sizeof(st->expanded) - 1);
    int i = 0;
    uint8_t first_vowel_in_word = 1;

    while (i < tlen && st->queue_len < SAY_MAX_PHONES - 4) {
        char c = txt[i];
        // Raw phoneme bracket mode: [HH EH L OW]
        if (c == '[') {
            i++;
            while (i < tlen && txt[i] != ']') {
                while (i < tlen && txt[i] == ' ') i++;
                if (i >= tlen || txt[i] == ']') break;
                char tok[3] = { 0, 0, 0 };
                int tl = 0;
                while (i < tlen && is_alpha(txt[i]) && tl < 2) {
                    tok[tl++] = txt[i++];
                }
                while (i < tlen && is_alpha(txt[i])) i++;
                for (uint32_t k = 0; k < NUM_PHONE_NAMES; k++) {
                    if (kPhoneNames[k].name[0] == tok[0] && kPhoneNames[k].name[1] == tok[1]) {
                        push_phone(st, kPhoneNames[k].id, 0);
                        break;
                    }
                }
            }
            if (i < tlen && txt[i] == ']') i++;
            continue;
        }

        if (!is_alpha(c)) {
            first_vowel_in_word = 1;
            if (c == ',' || c == ';' || c == ':') {
                push_phone(st, PH_PAU_COMMA, 0);
            } else if (c == '.' || c == '!' || c == '?') {
                push_phone(st, PH_PAU_PERIOD, 0);
            } else if (c == ' ' || c == '-') {
                if (st->queue_len > 0 && st->queue[st->queue_len - 1].phone_id < PH_PAU_WORD) {
                    push_phone(st, PH_PAU_WORD, 0);
                }
            }
            i++;
            continue;
        }

        // Special full word: AMIGA (5 phonemes)
        if (i + 5 <= tlen && (i == 0 || !is_alpha(txt[i - 1])) &&
            (i + 5 == tlen || !is_alpha(txt[i + 5])) &&
            txt[i] == 'A' && txt[i + 1] == 'M' && txt[i + 2] == 'I' &&
            txt[i + 3] == 'G' && txt[i + 4] == 'A') {
            push_phone(st, PH_AX, 0);
            push_phone(st, PH_M, 0);
            push_phone(st, PH_IY, 1);
            push_phone(st, PH_G, 0);
            push_phone(st, PH_AX, 0);
            i += 5;
            first_vowel_in_word = 0;
            continue;
        }

        // Match against LTS rules
        int matched = 0;
        for (uint32_t r = 0; r < NUM_LTS_RULES; r++) {
            const LtsRule* rule = &kLtsRules[r];
            int mlen = (int)str_len_max(rule->mid, 7);
            if (mlen <= 0 || i + mlen > tlen) continue;

            int eq = 1;
            for (int k = 0; k < mlen; k++) {
                if (txt[i + k] != rule->mid[k]) { eq = 0; break; }
            }
            if (!eq) continue;

            if (!match_left(rule->left, txt, i)) continue;
            if (!match_right(rule->right, txt, i + mlen, tlen)) continue;

            for (int k = 0; k < 4; k++) {
                uint8_t ph = rule->out[k];
                if (ph == 0) break;
                uint8_t stress = 0;
                if (first_vowel_in_word && (kPhoneTable[ph].flags & PF_VOWEL)) {
                    stress = 1;
                    first_vowel_in_word = 0;
                }
                push_phone(st, ph, stress);
            }
            i += mlen;
            matched = 1;
            break;
        }
        if (!matched) {
            i++;
        }
    }

    // Trailing release silence
    push_phone(st, PH_PAU_WORD, 0);
}

// ============================================================================
// Klatt-Style Formant Resonator DSP
// ============================================================================
static inline void resonator_set(Resonator* r, float freq, float bw) {
    if (!s_host || !s_host->expf || !s_host->cosf) return;
    if (freq < 80.0f) freq = 80.0f;
    if (freq > 9500.0f) freq = 9500.0f;
    if (bw < 35.0f) bw = 35.0f;
    float r_mag = s_host->expf(-SAY_PI * bw / SAY_INTERNAL_SR);
    r->c = -(r_mag * r_mag);
    r->b = 2.0f * r_mag * s_host->cosf(2.0f * SAY_PI * freq / SAY_INTERNAL_SR);
    r->a = 1.0f - r->b - r->c;
}

static inline float resonator_tick(Resonator* r, float x) {
    float y = r->a * x + r->b * r->y1 + r->c * r->y2;
    r->y2 = r->y1;
    r->y1 = y;
    return y;
}

static inline float next_noise(SayState* st) {
    st->rng_state = st->rng_state * 1664525u + 1013904223u;
    int32_t v = (int32_t)(st->rng_state >> 8) - 8388608;
    return (float)v * (1.0f / 8388608.0f);
}

static void prepare_utterance(void) {
    if (!s_st) return;
    const char* txt = (s_host && s_host->get_text_param)
                        ? s_host->get_text_param(0)
                        : "HELLO TRACKER";
    text_to_phonemes(s_st, txt);

    // Inform host of exact output length (2x internal 22.05 kHz samples -> 44.1 kHz)
    uint32_t out_samples = s_st->total_internal_samples * 2u;
    if (out_samples < 4410u) out_samples = 4410u;
    if (out_samples > SAY_MAX_SAMPLES) out_samples = SAY_MAX_SAMPLES;
    if (s_host && s_host->set_extra_samples) {
        s_host->set_extra_samples(0u, out_samples);
    }

    s_st->queue_pos = 0;
    s_st->phone_sample_idx = 0;
    s_st->rendered_internal_samples = 0;
    s_st->cur_f1 = 500.0f;  s_st->cur_f2 = 1500.0f; s_st->cur_f3 = 2500.0f;
    s_st->cur_bw1 = 100.0f; s_st->cur_bw2 = 150.0f; s_st->cur_bw3 = 200.0f;
    s_st->cur_fr_f = 3000.0f; s_st->cur_fr_bw = 1000.0f;
    s_st->cur_v_gain = 0.0f; s_st->cur_n_gain = 0.0f;
    s_st->cur_pitch = s_pitch * (s_voice ? 1.55f : 1.0f);

    if (s_host && s_host->memset) {
        s_host->memset(&s_st->r1, 0, sizeof(Resonator));
        s_host->memset(&s_st->r2, 0, sizeof(Resonator));
        s_host->memset(&s_st->r3, 0, sizeof(Resonator));
        s_host->memset(&s_st->r4, 0, sizeof(Resonator));
        s_host->memset(&s_st->r_fric, 0, sizeof(Resonator));
    }
    resonator_set(&s_st->r4, s_voice ? 3900.0f : 3450.0f, 260.0f);

    s_st->glot_phase = 0.0f;
    s_st->prev_glot = 0.0f;
    s_st->lp_state = 0.0f;
    s_st->prev_internal_sample = 0.0f;
    s_st->curr_internal_sample = 0.0f;
    s_st->upsample_phase = 0;
    s_st->lofi_hold_count = 0;
    s_st->lofi_held_sample = 0.0f;
    s_st->rng_state = 0x12345678u;
    s_st->prepared = 1;
}

static float render_internal_sample(SayState* st) {
    if (st->queue_pos >= st->queue_len) {
        return 0.0f;
    }

    const QueuedPhone* qp = &st->queue[st->queue_pos];
    const PhoneDef* pd = &kPhoneTable[qp->phone_id];

    // Vocal tract scaling: Male (1.0) vs Female (1.18) + Throat/Mouth knobs
    float sex_scale    = s_voice ? 1.18f : 1.0f;
    float throat_scale = (s_throat / 100.0f) * sex_scale;
    float mouth_scale  = (s_mouth  / 100.0f) * sex_scale;

    float tgt_f1  = (float)pd->f1 * throat_scale;
    float tgt_f2  = (float)pd->f2 * mouth_scale;
    float tgt_f3  = (float)pd->f3 * mouth_scale;
    float tgt_bw1 = (float)pd->bw1;
    float tgt_bw2 = (float)pd->bw2;
    float tgt_bw3 = (float)pd->bw3;
    float tgt_frf = (float)pd->fric_f * (s_voice ? 1.08f : 1.0f);
    float tgt_frb = (float)pd->fric_bw;

    float tgt_v = (float)pd->v_gain * 0.01f;
    float tgt_n = (float)pd->n_gain * 0.01f;

    // Stop consonants: first 55% is closure (quiet/voice-bar), final 45% is burst
    if (pd->flags & PF_STOP) {
        float prog = (float)st->phone_sample_idx / (float)(qp->dur_samples > 0 ? qp->dur_samples : 1);
        if (prog < 0.55f) {
            tgt_n = 0.0f;
            tgt_v = (pd->flags & PF_VOICED) ? 0.18f : 0.0f;
        } else if (prog < 0.80f) {
            // Crisp plosive burst!
            tgt_n = (float)pd->n_gain * 0.012f;
            tgt_v = (pd->flags & PF_VOICED) ? 0.35f : 0.0f;
        } else {
            // Aspiration / voice onset transition
            tgt_n = (float)pd->n_gain * 0.004f;
            tgt_v = (pd->flags & PF_VOICED) ? 0.55f : 0.0f;
        }
    }

    // Whisper mode: replace voicing with soft aspirated airflow through formants
    int is_whisper = (s_mode == 2);
    float asp_amp = 0.0f;
    if (is_whisper) {
        asp_amp = tgt_v * 0.65f;
        tgt_v = 0.0f;
    } else if (pd->flags & PF_ASPIRATED) {
        asp_amp = tgt_n * 0.70f;
        tgt_n = 0.0f;
    }

    // Smooth coarticulation (update coefficients every 16 internal samples = ~0.72 ms)
    if ((st->rendered_internal_samples & 15u) == 0u) {
        float alpha = (pd->flags & PF_STOP) ? 0.35f : 0.14f;
        st->cur_f1  += (tgt_f1  - st->cur_f1)  * alpha;
        st->cur_f2  += (tgt_f2  - st->cur_f2)  * alpha;
        st->cur_f3  += (tgt_f3  - st->cur_f3)  * alpha;
        st->cur_bw1 += (tgt_bw1 - st->cur_bw1) * alpha;
        st->cur_bw2 += (tgt_bw2 - st->cur_bw2) * alpha;
        st->cur_bw3 += (tgt_bw3 - st->cur_bw3) * alpha;
        st->cur_fr_f  += (tgt_frf - st->cur_fr_f) * 0.25f;
        st->cur_fr_bw += (tgt_frb - st->cur_fr_bw) * 0.25f;

        resonator_set(&st->r1, st->cur_f1, st->cur_bw1);
        resonator_set(&st->r2, st->cur_f2, st->cur_bw2);
        resonator_set(&st->r3, st->cur_f3, st->cur_bw3);
        resonator_set(&st->r_fric, st->cur_fr_f, st->cur_fr_bw);

        // Pitch contour (Natural vs Robot)
        float base_f0 = s_pitch * (s_voice ? 1.55f : 1.0f);
        if (base_f0 > 450.0f) base_f0 = 450.0f;
        float tgt_pitch = base_f0;
        if (s_mode == 0) {
            float utt_prog = (st->total_internal_samples > 0)
                               ? ((float)st->rendered_internal_samples / (float)st->total_internal_samples)
                               : 0.0f;
            // Natural sentence declination, or rising intonation for questions
            float contour = 1.08f - 0.20f * utt_prog;
            if (st->is_question && utt_prog > 0.72f) {
                float q = (utt_prog - 0.72f) / 0.28f;
                contour += 0.34f * q;
            }
            if (qp->stress) contour += 0.07f;
            tgt_pitch = base_f0 * contour;
        }
        st->cur_pitch += (tgt_pitch - st->cur_pitch) * 0.12f;
    }

    // Per-sample smooth amplitude ramps to prevent clicks
    st->cur_v_gain += (tgt_v - st->cur_v_gain) * 0.025f;
    st->cur_n_gain += (tgt_n - st->cur_n_gain) * 0.035f;

    // Glottal pulse generator (Rosenberg-style opening/closing + lip radiation diff)
    st->glot_phase += st->cur_pitch / SAY_INTERNAL_SR;
    if (st->glot_phase >= 1.0f) st->glot_phase -= 1.0f;

    float glot = 0.0f;
    if (st->glot_phase < 0.40f) {
        float t = st->glot_phase / 0.40f;
        glot = 0.5f * (1.0f - s_host->cosf(SAY_PI * t));
    } else if (st->glot_phase < 0.56f) {
        float t = (st->glot_phase - 0.40f) / 0.16f;
        glot = s_host->cosf(0.5f * SAY_PI * t);
    }
    // Lip radiation first-difference (boosts F2/F3 clarity)
    float glot_rad = (glot - 0.88f * st->prev_glot) * 2.4f;
    st->prev_glot = glot;

    float white = next_noise(st);

    // Cascade vocal tract path (glottal + aspiration)
    float tract_in = glot_rad * st->cur_v_gain + white * asp_amp * 0.45f;
    float y = resonator_tick(&st->r1, tract_in);
    y = resonator_tick(&st->r2, y);
    y = resonator_tick(&st->r3, y);
    y = resonator_tick(&st->r4, y);

    // Parallel fricative/burst noise path
    float fric = resonator_tick(&st->r_fric, white * st->cur_n_gain * 0.55f);

    float out = y * 0.55f + fric * 0.65f;

    // Advance phoneme position
    st->phone_sample_idx++;
    st->rendered_internal_samples++;
    if (st->phone_sample_idx >= qp->dur_samples) {
        st->phone_sample_idx = 0;
        st->queue_pos++;
    }

    return out;
}

// ============================================================================
// .ptfx Plugin Entry Points
// ============================================================================
void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host) {
    s_host = host;
    s_st = (scratch_mem && scratch_size >= sizeof(SayState)) ? (SayState*)scratch_mem : 0;
    s_voice  = 0;
    s_mode   = 0;
    s_pitch  = 110.0f;
    s_speed  = 150.0f;
    s_mouth  = 100.0f;
    s_throat = 100.0f;
    s_grime  = 1;
    if (s_st && s_host && s_host->memset) {
        s_host->memset(s_st, 0, sizeof(SayState));
    }
}

void plugin_set_param(uint8_t index, float value) {
    if      (index == 1) s_voice  = (uint8_t)(value + 0.5f);
    else if (index == 2) s_mode   = (uint8_t)(value + 0.5f);
    else if (index == 3) s_pitch  = value;
    else if (index == 4) s_speed  = value;
    else if (index == 5) s_mouth  = value;
    else if (index == 6) s_throat = value;
    else if (index == 7) s_grime  = (uint8_t)(value + 0.5f);
    if (s_st) {
        s_st->prepared = 0;
    }
    // Recompute expected length whenever parameters or text change
    prepare_utterance();
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
    (void)in;
    if (!s_st || !out || !out[0]) return 0;

    if (!s_st->prepared) {
        prepare_utterance();
    }
    if (s_st->queue_pos >= s_st->queue_len) {
        return 0;
    }

    uint32_t produced = 0;
    for (uint32_t i = 0; i < samples; i++) {
        if (s_st->queue_pos >= s_st->queue_len && s_st->upsample_phase == 0) {
            break;
        }

        float sample_val;
        if (s_st->upsample_phase == 0) {
            s_st->prev_internal_sample = s_st->curr_internal_sample;
            s_st->curr_internal_sample = render_internal_sample(s_st);
            s_st->upsample_phase = 1;
            sample_val = (s_grime == 0)
                           ? 0.5f * (s_st->prev_internal_sample + s_st->curr_internal_sample)
                           : s_st->curr_internal_sample;
        } else {
            s_st->upsample_phase = 0;
            sample_val = s_st->curr_internal_sample;
        }

        // Soft saturation for loud formant peaks with punchy sample level
        if (s_host && s_host->tanhf) {
            sample_val = s_host->tanhf(sample_val * 1.85f);
        }

        // Grime modes: 0=Clean, 1=Amiga 8-bit Paula DAC + gentle 4kHz filter, 2=Lo-Fi 11kHz/6-bit
        if (s_grime == 1) {
            // Amiga 8-bit signed PCM quantization + subtle warm analog output filter
            float q = (float)((int32_t)(sample_val * 127.0f)) * (1.0f / 127.0f);
            s_st->lp_state += (q - s_st->lp_state) * 0.45f;
            sample_val = s_st->lp_state;
        } else if (s_grime == 2) {
            if (s_st->lofi_hold_count == 0) {
                s_st->lofi_held_sample = (float)((int32_t)(sample_val * 31.0f)) * (1.0f / 31.0f);
                s_st->lofi_hold_count = 3;
            } else {
                s_st->lofi_hold_count--;
            }
            sample_val = s_st->lofi_held_sample;
        }

        int16_t pcm = (int16_t)SATURATE_16BIT((int32_t)(sample_val * 31000.0f));
        for (uint8_t ch = 0; ch < channels; ch++) {
            if (out[ch]) out[ch][i] = pcm;
        }
        produced++;
    }

    return (int32_t)produced;
}
