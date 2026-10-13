/*
 * ui.cpp - A dark, iOS-styled four-function calculator for the CYD
 *          ESP32-2432S024, written in plain LVGL 9.
 *
 * ---------------------------------------------------------------------------
 * Resolution independence
 * ---------------------------------------------------------------------------
 * Nothing here is pinned to 240x320.  build_keypad() reads the live display
 * resolution and derives every dimension from it:
 *
 *   - margins and the gap between keys are fractions of the screen width;
 *   - the keypad is 4 columns x 5 rows of CIRCULAR keys, so the key diameter
 *     is computed from the available width, then clamped down if the resulting
 *     keypad would leave too little room for the number display;
 *   - the keypad is centred horizontally and pinned to the bottom, and the
 *     display fills whatever space is left at the top;
 *   - font sizes are chosen from the diameter (keys) and from how many digits
 *     are showing (display), picking the nearest built-in Montserrat size.
 *
 * The aspect ratio of 240x320 (3:4) is assumed as the design target, but the
 * same code lays out a proportionate keypad on a taller 320x480 (2:3) panel:
 * the keys grow with the width and the extra vertical space simply makes the
 * number display roomier, exactly as it does on the real thing.
 *
 * The colour scheme mirrors the dark iOS calculator:
 *   - background        : near-black
 *   - number keys       : dark gray, white glyphs
 *   - function keys (top): light gray, black glyphs   (backspace, C, %)
 *   - operator column   : orange, white glyphs        (/  x  -  +  =)
 */

#include "ui.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ colours */

#define COL_BG        lv_color_hex(0x000000)   /* screen background           */
#define COL_NUM       lv_color_hex(0x333333)   /* number keys                 */
#define COL_NUM_TXT   lv_color_hex(0xFFFFFF)
#define COL_FUNC      lv_color_hex(0xA5A5A5)   /* top-row function keys       */
#define COL_FUNC_TXT  lv_color_hex(0x000000)
#define COL_OP        lv_color_hex(0xFF9F0A)   /* operator column             */
#define COL_OP_TXT    lv_color_hex(0xFFFFFF)
#define COL_DISP_TXT  lv_color_hex(0xFFFFFF)   /* the number display          */

/* --------------------------------------------------------------- key tables */

/* What a key does.  For digits and operators the action code is just the
 * character itself, which keeps the calculator logic below very direct. */
typedef enum {
    KT_NUM,     /* dark gray number / decimal point key */
    KT_FUNC,    /* light gray top-row function key      */
    KT_OP,      /* orange operator key                  */
} key_kind_t;

typedef struct {
    const char *label;   /* glyph(s) drawn on the key                     */
    char        action;  /* code handed to on_key(): digit, '.', op, etc. */
    key_kind_t  kind;    /* colour group                                  */
} key_def_t;

/* 4 columns x 5 rows, laid out exactly like the screenshot:
 *
 *    <x]   C    %    /
 *    7     8    9    x
 *    4     5    6    -
 *    1     2    3    +
 *    +/-   0    .    =
 *
 * The operator glyphs are ASCII ("/ x - +") rather than the Unicode math
 * symbols, because LVGL's built-in Montserrat fonts only carry ASCII (plus the
 * LV_SYMBOL_* icons); a real divide/multiply sign would render as a blank box.
 * LV_SYMBOL_BACKSPACE is one of those built-in icons, so it draws correctly.
 */
#define COLS 4
#define ROWS 5
static const key_def_t KEYS[ROWS][COLS] = {
    { {LV_SYMBOL_BACKSPACE, 'B', KT_FUNC}, {"C", 'C', KT_FUNC}, {"%", '%', KT_FUNC}, {"/", '/', KT_OP} },
    { {"7", '7', KT_NUM},  {"8", '8', KT_NUM}, {"9", '9', KT_NUM}, {"x", '*', KT_OP} },
    { {"4", '4', KT_NUM},  {"5", '5', KT_NUM}, {"6", '6', KT_NUM}, {"-", '-', KT_OP} },
    { {"1", '1', KT_NUM},  {"2", '2', KT_NUM}, {"3", '3', KT_NUM}, {"+", '+', KT_OP} },
    { {"+/-", 'N', KT_NUM},{"0", '0', KT_NUM}, {".", '.', KT_NUM}, {"=", '=', KT_OP} },
};

/* ----------------------------------------------------------- font selection */

/* All of these Montserrat sizes are enabled in the bundled lv_conf.h. */
static const lv_font_t *const FONTS[] = {
    &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
    &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_24,
    &lv_font_montserrat_28, &lv_font_montserrat_32, &lv_font_montserrat_36,
    &lv_font_montserrat_40, &lv_font_montserrat_44, &lv_font_montserrat_48,
};
static const int FONT_PX[] = { 12, 14, 16, 18, 20, 24, 28, 32, 36, 40, 44, 48 };
static const int FONT_N = sizeof(FONT_PX) / sizeof(FONT_PX[0]);

/* Nearest available font whose pixel size is <= want (clamped to the range). */
static const lv_font_t *font_for_px(int want) {
    int best = 0;
    for (int i = 0; i < FONT_N; i++) {
        if (FONT_PX[i] <= want) best = i;
    }
    return FONTS[best];
}

/* ------------------------------------------------------------- widget state */

static lv_obj_t *g_display;                 /* the big right-aligned number  */
static int       g_disp_w;                  /* width available to the number */
static int       g_disp_px;                 /* "natural" display font size   */

/* ------------------------------------------------------- calculator engine */

/* A plain immediate-execution calculator: type a number, an operator, another
 * number, then = (or chain operators, each applying the previous one). */
static char   g_entry[20] = "0";  /* digits the user is currently typing      */
static double g_acc       = 0;    /* running accumulator                      */
static char   g_op        = 0;    /* pending operator, 0 for none             */
static bool   g_fresh     = true; /* next digit starts a brand-new entry      */
static bool   g_error     = false;/* divide-by-zero / overflow latched        */

/* Insert thousands separators into the integer part of a plain numeric string
 * ("-1234567.89" -> "-1,234,567.89").  Fractional digits are left untouched. */
static void group_thousands(const char *in, char *out, size_t outsz) {
    size_t oi = 0;
    const char *p = in;

    if (*p == '-') { if (oi + 1 < outsz) out[oi++] = *p; p++; }

    const char *dot = strchr(p, '.');
    size_t int_len = dot ? (size_t)(dot - p) : strlen(p);

    for (size_t i = 0; i < int_len; i++) {
        if (i > 0 && (int_len - i) % 3 == 0) {
            if (oi + 1 < outsz) out[oi++] = ',';
        }
        if (oi + 1 < outsz) out[oi++] = p[i];
    }
    /* copy the decimal point and any fractional digits verbatim */
    for (const char *q = p + int_len; *q && oi + 1 < outsz; q++) out[oi++] = *q;
    out[oi] = '\0';
}

/* Format a double into a plain (un-grouped) string: fixed-point, trailing
 * zeros trimmed, no scientific notation.  Falls back to "Error" if the value
 * is out of the range we can show legibly. */
static void format_double(double v, char *out, size_t outsz) {
    if (isnan(v) || isinf(v) || fabs(v) >= 1e12) { snprintf(out, outsz, "Error"); return; }
    if (v == 0) v = 0;  /* normalise -0 */

    snprintf(out, outsz, "%.6f", v);

    /* trim trailing zeros, then a dangling decimal point */
    char *dot = strchr(out, '.');
    if (dot) {
        char *end = out + strlen(out) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *end = '\0';
    }
}

/* Push g_entry (or an error message) to the display, with grouping and a font
 * size that shrinks as the number grows so it keeps fitting the width. */
static void refresh_display(void) {
    char grouped[40];
    if (g_error) {
        snprintf(grouped, sizeof(grouped), "Error");
    } else {
        group_thousands(g_entry, grouped, sizeof(grouped));
    }

    /* Shrink the font once the string gets long, so big numbers still fit. */
    int len = (int)strlen(grouped);
    int px  = g_disp_px;
    if (len > 9)  px = g_disp_px * 9 / len;     /* roughly proportional      */
    if (px < 14)  px = 14;
    lv_obj_set_style_text_font(g_display, font_for_px(px), 0);

    lv_label_set_text(g_display, grouped);
}

/* Apply a pending binary operator: acc = acc <op> rhs. */
static double apply_op(double acc, double rhs, char op) {
    switch (op) {
        case '+': return acc + rhs;
        case '-': return acc - rhs;
        case '*': return acc * rhs;
        case '/':
            if (rhs == 0) { g_error = true; return 0; }
            return acc / rhs;
    }
    return rhs;
}

static void entry_from_double(double v) {
    format_double(v, g_entry, sizeof(g_entry));
}

static void append_digit(char d) {
    if (g_fresh) { strcpy(g_entry, "0"); g_fresh = false; }
    size_t len = strlen(g_entry);
    if (strcmp(g_entry, "0") == 0) {        /* replace a lone leading zero    */
        g_entry[0] = d; g_entry[1] = '\0';
    } else if (len < sizeof(g_entry) - 1) { /* cap length to avoid overflow   */
        g_entry[len] = d; g_entry[len + 1] = '\0';
    }
}

static void append_dot(void) {
    if (g_fresh) { strcpy(g_entry, "0"); g_fresh = false; }
    if (!strchr(g_entry, '.') && strlen(g_entry) < sizeof(g_entry) - 1) {
        strcat(g_entry, ".");
    }
}

static void set_operator(char op) {
    double rhs = atof(g_entry);
    if (g_op && !g_fresh) {
        g_acc = apply_op(g_acc, rhs, g_op);  /* chain: fold the previous op   */
    } else {
        g_acc = rhs;
    }
    g_op = op;
    g_fresh = true;
    if (!g_error) entry_from_double(g_acc);  /* show the running result       */
}

static void do_equals(void) {
    double rhs = atof(g_entry);
    if (g_op) g_acc = apply_op(g_acc, rhs, g_op);
    else      g_acc = rhs;
    g_op = 0;
    g_fresh = true;
    if (!g_error) entry_from_double(g_acc);
}

static void clear_all(void) {
    strcpy(g_entry, "0");
    g_acc = 0; g_op = 0; g_fresh = true; g_error = false;
}

/* Central dispatch for every key.  `action` is the key_def_t.action code. */
static void on_key(char action) {
    /* In an error state, only Clear (or backspace) gets us out. */
    if (g_error && action != 'C' && action != 'B') return;

    switch (action) {
        case 'C': clear_all(); break;
        case 'B':                                   /* backspace             */
            if (g_error) { clear_all(); break; }
            if (!g_fresh) {
                size_t len = strlen(g_entry);
                if (len > 1) g_entry[len - 1] = '\0';
                else         strcpy(g_entry, "0");
                if (strcmp(g_entry, "0") == 0) g_fresh = true;
            }
            break;
        case 'N':                                   /* +/- toggle sign       */
            if (strcmp(g_entry, "0") != 0) {
                if (g_entry[0] == '-') memmove(g_entry, g_entry + 1, strlen(g_entry));
                else { memmove(g_entry + 1, g_entry, strlen(g_entry) + 1); g_entry[0] = '-'; }
            }
            break;
        case '%':                                   /* percent               */
            entry_from_double(atof(g_entry) / 100.0);
            g_fresh = true;
            break;
        case '.': append_dot(); break;
        case '+': case '-': case '*': case '/': set_operator(action); break;
        case '=': do_equals(); break;
        default:  if (action >= '0' && action <= '9') append_digit(action); break;
    }
    refresh_display();
}

static void key_event_cb(lv_event_t *e) {
    char action = (char)(intptr_t)lv_event_get_user_data(e);
    on_key(action);
}

/* ------------------------------------------------------------- UI building */

/* Style one key according to its colour group, and make it a circle. */
static void style_key(lv_obj_t *btn, const key_def_t *k, int diameter) {
    lv_color_t bg, txt;
    switch (k->kind) {
        case KT_FUNC: bg = COL_FUNC; txt = COL_FUNC_TXT; break;
        case KT_OP:   bg = COL_OP;   txt = COL_OP_TXT;   break;
        default:      bg = COL_NUM;  txt = COL_NUM_TXT;  break;
    }

    lv_obj_set_size(btn, diameter, diameter);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    /* Lighten slightly while pressed, for a bit of tactile feedback. */
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, k->label);
    lv_obj_set_style_text_color(lbl, txt, 0);
    lv_obj_set_style_text_font(lbl, font_for_px((int)(diameter * 0.42f)), 0);
    lv_obj_center(lbl);
}

/* Lay out the display + keypad from the live resolution (see file header). */
static void build_keypad(lv_obj_t *scr) {
    lv_display_t *disp = lv_display_get_default();
    int W = lv_display_get_horizontal_resolution(disp);
    int H = lv_display_get_vertical_resolution(disp);

    int margin = (int)(W * 0.04f);   /* outer margin            */
    int gap    = (int)(W * 0.03f);   /* gap between keys        */
    if (margin < 4) margin = 4;
    if (gap < 3)    gap = 3;

    /* Key diameter from the width: 4 keys + 3 gaps span the usable width. */
    int d = (W - 2 * margin - (COLS - 1) * gap) / COLS;

    /* If a width-sized keypad would crowd out the display, shrink the keys so
     * at least ~16% of the height is left for the number readout. */
    int min_disp = (int)(H * 0.16f);
    int grid_h   = ROWS * d + (ROWS - 1) * gap;
    int max_grid = H - margin - min_disp;
    if (grid_h > max_grid) {
        d = (max_grid - (ROWS - 1) * gap) / ROWS;
        grid_h = ROWS * d + (ROWS - 1) * gap;
    }

    int grid_w = COLS * d + (COLS - 1) * gap;     /* keypad is centred...     */
    int x0     = (W - grid_w) / 2;
    int y0     = H - grid_h - margin;             /* ...and pinned to bottom  */

    /* The display owns everything above the keypad. */
    g_disp_w  = W - 2 * margin;
    g_disp_px = (int)(d * 1.1f);                  /* scaled to the keys       */
    if (g_disp_px > 48) g_disp_px = 48;

    g_display = lv_label_create(scr);
    lv_obj_set_width(g_display, g_disp_w);
    lv_obj_set_style_text_color(g_display, COL_DISP_TXT, 0);
    lv_obj_set_style_text_align(g_display, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(g_display, LV_LABEL_LONG_CLIP);
    /* Sit the number just above the keypad, right-aligned within the margins. */
    lv_obj_set_pos(g_display, margin, y0 - g_disp_px - margin / 2);
    lv_obj_set_height(g_display, g_disp_px + margin);

    /* Lay out the 4x5 grid of circular keys. */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            const key_def_t *k = &KEYS[r][c];
            lv_obj_t *btn = lv_button_create(scr);
            lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(btn, x0 + c * (d + gap), y0 + r * (d + gap));
            style_key(btn, k, d);
            lv_obj_add_event_cb(btn, key_event_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)k->action);
        }
    }

    refresh_display();
}

void ui_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    clear_all();
    build_keypad(scr);
}
