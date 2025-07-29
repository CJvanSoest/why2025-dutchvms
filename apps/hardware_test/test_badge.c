/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "badgevms/compositor.h"
#include "badgevms/event.h"
#include "badgevms/framebuffer.h"
#include "badgevms/keyboard.h"
#include "thirdparty/microui.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <time.h>

static uint8_t const font_8x8[96][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // Space (32)
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, // ! (33)
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // " (34)
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00}, // # (35)
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, // $ (36)
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00}, // % (37)
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, // & (38)
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, // ' (39)
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, // ( (40)
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00}, // ) (41)
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, // * (42)
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00}, // + (43)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x06, 0x00}, // , (44)
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00}, // - (45)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, // . (46)
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00}, // / (47)
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, // 0 (48)
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00}, // 1 (49)
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, // 2 (50)
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00}, // 3 (51)
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, // 4 (52)
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00}, // 5 (53)
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, // 6 (54)
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00}, // 7 (55)
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, // 8 (56)
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00}, // 9 (57)
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, // : (58)
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x06, 0x00}, // ; (59)
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, // < (60)
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00}, // = (61)
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, // > (62)
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00}, // ? (63)
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, // @ (64)
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, // A (65)
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, // B (66)
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00}, // C (67)
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, // D (68)
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00}, // E (69)
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, // F (70)
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00}, // G (71)
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, // H (72)
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // I (73)
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, // J (74)
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, // K (75)
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, // L (76)
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00}, // M (77)
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, // N (78)
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00}, // O (79)
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, // P (80)
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00}, // Q (81)
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, // R (82)
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00}, // S (83)
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // T (84)
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00}, // U (85)
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, // V (86)
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, // W (87)
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, // X (88)
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00}, // Y (89)
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, // Z (90)
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00}, // [ (91)
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, // \ (92)
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00}, // ] (93)
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, // ^ (94)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00}, // _ (95)
    {0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, // ` (96)
    {0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00}, // a (97)
    {0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00}, // b (98)
    {0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00}, // c (99)
    {0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6E, 0x00}, // d (100)
    {0x00, 0x00, 0x1E, 0x33, 0x3f, 0x03, 0x1E, 0x00}, // e (101)
    {0x1C, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0F, 0x00}, // f (102)
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F}, // g (103)
    {0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00}, // h (104)
    {0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // i (105)
    {0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E}, // j (106)
    {0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00}, // k (107)
    {0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, // l (108)
    {0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00}, // m (109)
    {0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00}, // n (110)
    {0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00}, // o (111)
    {0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F}, // p (112)
    {0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78}, // q (113)
    {0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00}, // r (114)
    {0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00}, // s (115)
    {0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00}, // t (116)
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00}, // u (117)
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, // v (118)
    {0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00}, // w (119)
    {0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00}, // x (120)
    {0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F}, // y (121)
    {0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00}, // z (122)
    {0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00}, // { (123)
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, // | (124)
    {0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00}, // } (125)
    {0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // ~ (126)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // DEL (127)
};

int mu_text_width(mu_Font font, char const *text, int len) {
    if (len == -1)
        len = strlen(text);

    int width = 0;
    for (int i = 0; i < len;) {
        // Check for multi-byte UTF-8 sequences
        if ((unsigned char)text[i] >= 0x80) {
            char symbol[8]  = {0};
            int  symbol_len = 0;

            while (i + symbol_len < len && text[i + symbol_len] &&
                   ((unsigned char)text[i + symbol_len] >= 0x80 ||
                    (symbol_len > 0 && (unsigned char)text[i + symbol_len] >= 0x80))) {
                symbol[symbol_len] = text[i + symbol_len];
                symbol_len++;
                if (symbol_len >= 7)
                    break;
            }

            if (symbol_len == 0 && ((unsigned char)text[i] >= 0x80)) {
                symbol_len = 1;
            }

            width += 8;
            i     += symbol_len > 0 ? symbol_len : 1;
        } else {
            width += 8;
            i++;
        }
    }
    return width;
}

int mu_text_height(mu_Font font) {
    return 8;
}

static uint8_t const special_symbols[][8] = {
    // □ (square)
    {0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF},
    // △ (triangle)
    {0x08, 0x1C, 0x1C, 0x36, 0x36, 0x63, 0x7F, 0x00},
    // ✕ (cross/X)
    {0x63, 0x36, 0x1C, 0x08, 0x1C, 0x36, 0x63, 0x00},
    // ○ (circle)
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},
    // ☁ (cloud)
    {0x00, 0x1C, 0x36, 0x36, 0x7F, 0x00, 0x00, 0x00},
    // ◆ (diamond)
    {0x08, 0x1C, 0x36, 0x63, 0x36, 0x1C, 0x08, 0x00},
    // ← (left arrow)
    {0x00, 0x08, 0x0C, 0x7E, 0x0C, 0x08, 0x00, 0x00},
    // → (right arrow)
    {0x00, 0x10, 0x30, 0x7E, 0x30, 0x10, 0x00, 0x00},
    // ↑ (up arrow)
    {0x08, 0x1C, 0x36, 0x63, 0x08, 0x08, 0x08, 0x00},
    // ↓ (down arrow)
    {0x08, 0x08, 0x08, 0x63, 0x36, 0x1C, 0x08, 0x00},
    // ⟲ (rotate)
    {0x00, 0x1E, 0x06, 0x66, 0x66, 0x60, 0x78, 0x00},
    // ⌫ (backspace)
    {0x00, 0x60, 0x78, 0x7E, 0x78, 0x60, 0x00, 0x00},
    // ↵ (enter/return)
    {0x00, 0x01, 0x01, 0x7F, 0x01, 0x01, 0x00, 0x00},
    // ⇧ (shift)
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x7F, 0x00, 0x00},
};

int get_special_symbol_index(char const *symbol) {
    if (strcmp(symbol, "□") == 0)
        return 0;
    if (strcmp(symbol, "△") == 0)
        return 1;
    if (strcmp(symbol, "✕") == 0)
        return 2;
    if (strcmp(symbol, "○") == 0)
        return 3;
    if (strcmp(symbol, "☁") == 0)
        return 4;
    if (strcmp(symbol, "◆") == 0)
        return 5;
    if (strcmp(symbol, "←") == 0)
        return 6;
    if (strcmp(symbol, "→") == 0)
        return 7;
    if (strcmp(symbol, "↑") == 0)
        return 8;
    if (strcmp(symbol, "↓") == 0)
        return 9;
    if (strcmp(symbol, "⟲") == 0)
        return 10;
    if (strcmp(symbol, "⌫") == 0)
        return 11;
    if (strcmp(symbol, "↵") == 0)
        return 12;
    if (strcmp(symbol, "⇧") == 0)
        return 13;
    return -1;
}

void draw_special_symbol(framebuffer_t *fb, int symbol_index, int x, int y, uint16_t color) {
    if (symbol_index < 0 || symbol_index >= 15)
        return;

    uint8_t const *char_data = special_symbols[symbol_index];

    for (int row = 0; row < 8; row++) {
        uint8_t byte = char_data[row];
        for (int col = 0; col < 8; col++) {
            if (byte & (0x01 << col)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < fb->w && py >= 0 && py < fb->h) {
                    fb->pixels[py * fb->w + px] = color;
                }
            }
        }
    }
}

void draw_rect(framebuffer_t *fb, mu_Rect rect, mu_Color color) {
    uint16_t color565 = rgb888_to_rgb565(color.r, color.g, color.b);

    for (int y = rect.y; y < rect.y + rect.h; y++) {
        for (int x = rect.x; x < rect.x + rect.w; x++) {
            if (x >= 0 && x < fb->w && y >= 0 && y < fb->h) {
                fb->pixels[y * fb->w + x] = color565;
            }
        }
    }
}

void draw_char(framebuffer_t *fb, char c, int x, int y, uint16_t color) {
    if (c < 32 || c > 127)
        return;

    uint8_t const *char_data = font_8x8[c - 32];

    for (int row = 0; row < 8; row++) {
        uint8_t byte = char_data[row];
        for (int col = 0; col < 8; col++) {
            if (byte & (0x01 << col)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < fb->w && py >= 0 && py < fb->h) {
                    fb->pixels[py * fb->w + px] = color;
                }
            }
        }
    }
}

void draw_text(framebuffer_t *fb, char const *text, int x, int y, mu_Color color) {
    uint16_t color565  = rgb888_to_rgb565(color.r, color.g, color.b);
    int      current_x = x;
    int      current_y = y;

    for (int i = 0; text[i];) {
        if (text[i] == '\n') {
            current_x  = x;
            current_y += 10;
            i++;
            continue;
        }

        // Check for multi-byte UTF-8 sequences
        if ((unsigned char)text[i] >= 0x80) {
            char symbol[8]  = {0};
            int  symbol_len = 0;

            while (text[i + symbol_len] && ((unsigned char)text[i + symbol_len] >= 0x80 ||
                                            (symbol_len > 0 && (unsigned char)text[i + symbol_len] >= 0x80))) {
                symbol[symbol_len] = text[i + symbol_len];
                symbol_len++;
                if (symbol_len >= 7)
                    break;
            }

            if (symbol_len == 0 && ((unsigned char)text[i] >= 0x80)) {
                symbol[0]  = text[i];
                symbol_len = 1;
            }

            int special_idx = get_special_symbol_index(symbol);
            if (special_idx >= 0) {
                draw_special_symbol(fb, special_idx, current_x, current_y, color565);
            } else {
                // Fallback
                draw_char(fb, '?', current_x, current_y, color565);
            }

            current_x += 8;
            i         += symbol_len > 0 ? symbol_len : 1;
        } else {
            // Regular ASCII character
            draw_char(fb, text[i], current_x, current_y, color565);
            current_x += 8;
            i++;
        }
    }
}

typedef struct {
    char name[32];
    char status[32];
    bool passed;
} test_result_t;

typedef enum {
    KEY_STATE_NEVER_PRESSED,
    KEY_STATE_CURRENTLY_PRESSED,
    KEY_STATE_PREVIOUSLY_PRESSED,
    KEY_STATE_STUCK_DOWN
} key_state_t;

typedef struct {
    keyboard_scancode_t scancode;
    char                label[8];
    key_state_t         state;
    uint64_t            last_press_time;
} key_info_t;

typedef struct {
    mu_Context     *ctx;
    framebuffer_t  *fb;
    window_handle_t window;

    test_result_t tests[8];
    int           num_tests;

    key_info_t row1[8];  // Function keys
    key_info_t row2[13]; // Number row
    key_info_t row3[13]; // QWERTY row
    key_info_t row4[13]; // ASDF row
    key_info_t row5[13]; // ZXCV row
    key_info_t row6[9];  // Bottom row

    char input_buffer[50];
} app_state_t;

void init_tests(app_state_t *app) {
    app->num_tests = 8;

    strcpy(app->tests[0].name, "I2C Bus");
    strcpy(app->tests[0].status, "Scan Success 4/4");
    app->tests[0].passed = true;

    strcpy(app->tests[1].name, "SPI Flash");
    strcpy(app->tests[1].status, "Test Passed");
    app->tests[1].passed = true;

    strcpy(app->tests[2].name, "Display");
    strcpy(app->tests[2].status, "Test Passed");
    app->tests[2].passed = true;

    strcpy(app->tests[3].name, "Audio");
    strcpy(app->tests[3].status, "Test Failed");
    app->tests[3].passed = false;

    strcpy(app->tests[4].name, "WiFi");
    strcpy(app->tests[4].status, "Connection OK");
    app->tests[4].passed = true;

    strcpy(app->tests[5].name, "USB");
    strcpy(app->tests[5].status, "Device Ready");
    app->tests[5].passed = true;

    strcpy(app->tests[6].name, "SD Card");
    strcpy(app->tests[6].status, "Mount Failed");
    app->tests[6].passed = false;

    strcpy(app->tests[7].name, "Sensors");
    strcpy(app->tests[7].status, "All Online");
    app->tests[7].passed = true;
}

// Initialize keyboard layout based on actual WHY2025 badge (from image)
void init_keyboard_layout(app_state_t *app) {
    // Row 1: Function keys
    keyboard_scancode_t const r1_scancodes[] = {
        KEY_SCANCODE_ESCAPE,
        KEY_SCANCODE_SQUARE,
        KEY_SCANCODE_TRIANGLE,
        KEY_SCANCODE_CROSS,
        KEY_SCANCODE_CIRCLE,
        KEY_SCANCODE_CLOUD,
        KEY_SCANCODE_DIAMOND,
        KEY_SCANCODE_BACKSPACE
    };
    char const *r1_labels[] = {"ESC", "□", "△", "✕", "○", "☁", "◆", "⌫"};

    for (int i = 0; i < 8; i++) {
        app->row1[i].scancode = r1_scancodes[i];
        strcpy(app->row1[i].label, r1_labels[i]);
        app->row1[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row1[i].last_press_time = 0;
    }

    // Row 2: Number row
    keyboard_scancode_t const r2_scancodes[] = {
        KEY_SCANCODE_GRAVE,
        KEY_SCANCODE_1,
        KEY_SCANCODE_2,
        KEY_SCANCODE_3,
        KEY_SCANCODE_4,
        KEY_SCANCODE_5,
        KEY_SCANCODE_6,
        KEY_SCANCODE_7,
        KEY_SCANCODE_8,
        KEY_SCANCODE_9,
        KEY_SCANCODE_0,
        KEY_SCANCODE_MINUS,
        KEY_SCANCODE_EQUALS
    };
    char const *r2_labels[] = {"~", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "="};

    for (int i = 0; i < 13; i++) {
        app->row2[i].scancode = r2_scancodes[i];
        strcpy(app->row2[i].label, r2_labels[i]);
        app->row2[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row2[i].last_press_time = 0;
    }

    // Row 3: QWERTY row
    keyboard_scancode_t const r3_scancodes[] = {
        KEY_SCANCODE_TAB,
        KEY_SCANCODE_Q,
        KEY_SCANCODE_W,
        KEY_SCANCODE_E,
        KEY_SCANCODE_R,
        KEY_SCANCODE_T,
        KEY_SCANCODE_Y,
        KEY_SCANCODE_U,
        KEY_SCANCODE_I,
        KEY_SCANCODE_O,
        KEY_SCANCODE_P,
        KEY_SCANCODE_LEFTBRACKET,
        KEY_SCANCODE_RIGHTBRACKET
    };
    char const *r3_labels[] = {"TAB", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]"};

    for (int i = 0; i < 13; i++) {
        app->row3[i].scancode = r3_scancodes[i];
        strcpy(app->row3[i].label, r3_labels[i]);
        app->row3[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row3[i].last_press_time = 0;
    }

    // Row 4: ASDF row
    keyboard_scancode_t const r4_scancodes[] = {
        KEY_SCANCODE_FN,
        KEY_SCANCODE_A,
        KEY_SCANCODE_S,
        KEY_SCANCODE_D,
        KEY_SCANCODE_F,
        KEY_SCANCODE_G,
        KEY_SCANCODE_H,
        KEY_SCANCODE_J,
        KEY_SCANCODE_K,
        KEY_SCANCODE_L,
        KEY_SCANCODE_SEMICOLON,
        KEY_SCANCODE_APOSTROPHE,
        KEY_SCANCODE_RETURN
    };
    char const *r4_labels[] = {"Fn", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "↵"};

    for (int i = 0; i < 13; i++) {
        app->row4[i].scancode = r4_scancodes[i];
        strcpy(app->row4[i].label, r4_labels[i]);
        app->row4[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row4[i].last_press_time = 0;
    }

    // Row 5: ZXCV row
    keyboard_scancode_t const r5_scancodes[] = {
        KEY_SCANCODE_LSHIFT,
        KEY_SCANCODE_Z,
        KEY_SCANCODE_X,
        KEY_SCANCODE_C,
        KEY_SCANCODE_V,
        KEY_SCANCODE_B,
        KEY_SCANCODE_N,
        KEY_SCANCODE_M,
        KEY_SCANCODE_COMMA,
        KEY_SCANCODE_PERIOD,
        KEY_SCANCODE_SLASH,
        KEY_SCANCODE_UP,
        KEY_SCANCODE_RSHIFT
    };
    char const *r5_labels[] = {"⇧", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "↑", "⇧"};

    for (int i = 0; i < 13; i++) {
        app->row5[i].scancode = r5_scancodes[i];
        strcpy(app->row5[i].label, r5_labels[i]);
        app->row5[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row5[i].last_press_time = 0;
    }

    // Row 6: Bottom row
    keyboard_scancode_t const r6_scancodes[] = {
        KEY_SCANCODE_LCTRL,
        KEY_SCANCODE_LGUI,
        KEY_SCANCODE_LALT,
        KEY_SCANCODE_BACKSLASH,
        KEY_SCANCODE_SPACE,
        KEY_SCANCODE_RALT,
        KEY_SCANCODE_LEFT,
        KEY_SCANCODE_DOWN,
        KEY_SCANCODE_RIGHT
    };
    char const *r6_labels[] = {"CTRL", "⟲", "ALT", "|", "SPACE", "ALT", "←", "↓", "→"};

    for (int i = 0; i < 9; i++) {
        app->row6[i].scancode = r6_scancodes[i];
        strcpy(app->row6[i].label, r6_labels[i]);
        app->row6[i].state           = KEY_STATE_NEVER_PRESSED;
        app->row6[i].last_press_time = 0;
    }
}

void handle_keyboard_event(app_state_t *app, keyboard_event_t *kbd_event) {
    key_info_t *found_key = NULL;

    for (int i = 0; i < 8 && !found_key; i++) {
        if (app->row1[i].scancode == kbd_event->scancode)
            found_key = &app->row1[i];
    }
    for (int i = 0; i < 13 && !found_key; i++) {
        if (app->row2[i].scancode == kbd_event->scancode)
            found_key = &app->row2[i];
    }
    for (int i = 0; i < 13 && !found_key; i++) {
        if (app->row3[i].scancode == kbd_event->scancode)
            found_key = &app->row3[i];
    }
    for (int i = 0; i < 13 && !found_key; i++) {
        if (app->row4[i].scancode == kbd_event->scancode)
            found_key = &app->row4[i];
    }
    for (int i = 0; i < 13 && !found_key; i++) {
        if (app->row5[i].scancode == kbd_event->scancode)
            found_key = &app->row5[i];
    }
    for (int i = 0; i < 9 && !found_key; i++) {
        if (app->row6[i].scancode == kbd_event->scancode)
            found_key = &app->row6[i];
    }

    if (found_key) {
        if (kbd_event->down) {
            if (found_key->state == KEY_STATE_NEVER_PRESSED || found_key->state == KEY_STATE_PREVIOUSLY_PRESSED) {
                found_key->state           = KEY_STATE_CURRENTLY_PRESSED;
                found_key->last_press_time = kbd_event->timestamp;
            }
        } else {
            if (found_key->state == KEY_STATE_CURRENTLY_PRESSED || found_key->state == KEY_STATE_STUCK_DOWN) {
                found_key->state = KEY_STATE_PREVIOUSLY_PRESSED;
            }
        }
    }

    int input_buffer_len = strlen(app->input_buffer);
    if (kbd_event->down && kbd_event->text && kbd_event->text != 0) {
        if (input_buffer_len < sizeof(app->input_buffer) - 1) {
            app->input_buffer[input_buffer_len]     = kbd_event->text;
            app->input_buffer[input_buffer_len + 1] = '\0';
            ++input_buffer_len;
        }
    }

    if (kbd_event->down && kbd_event->scancode == KEY_SCANCODE_BACKSPACE) {
        if (input_buffer_len > 2) {
            // We just wrote a backspace to the buffer.
            app->input_buffer[input_buffer_len - 2] = '\0';
            --input_buffer_len;
        }
    }

    if (input_buffer_len == sizeof(app->input_buffer) - 1) {
        app->input_buffer[0] = '\0';
    }
}

// TODO this is kinda broken, maybe even as a concept
void check_stuck_keys(app_state_t *app, uint64_t current_time) {
    uint64_t const STUCK_THRESHOLD = 5000000000ULL; // 5 seconds

    key_info_t *all_keys[]  = {app->row1, app->row2, app->row3, app->row4, app->row5, app->row6};
    int         row_sizes[] = {8, 13, 13, 13, 13, 9};

    for (int row = 0; row < 6; row++) {
        for (int i = 0; i < row_sizes[row]; i++) {
            key_info_t *key = &all_keys[row][i];
            if (key->state == KEY_STATE_CURRENTLY_PRESSED) {
                if (current_time - key->last_press_time > STUCK_THRESHOLD) {
                    key->state = KEY_STATE_STUCK_DOWN;
                }
            }
        }
    }
}

mu_Color get_key_color(key_state_t state) {
    switch (state) {
        case KEY_STATE_NEVER_PRESSED: return mu_color(64, 64, 64, 255);       // Dark gray
        case KEY_STATE_CURRENTLY_PRESSED: return mu_color(0, 255, 0, 255);    // Bright green
        case KEY_STATE_PREVIOUSLY_PRESSED: return mu_color(255, 255, 0, 255); // Yellow
        case KEY_STATE_STUCK_DOWN: return mu_color(255, 0, 0, 255);           // Red
        default: return mu_color(64, 64, 64, 255);
    }
}

void set_button_color(mu_Context *ctx, mu_Color color) {
    ctx->style->colors[MU_COLOR_BUTTON]      = color;
    ctx->style->colors[MU_COLOR_BUTTONHOVER] = color;
    ctx->style->colors[MU_COLOR_BUTTONFOCUS] = color;
}

void reset_button_colors(mu_Context *ctx) {
    ctx->style->colors[MU_COLOR_BUTTON]      = mu_color(68, 68, 68, 255);
    ctx->style->colors[MU_COLOR_BUTTONHOVER] = mu_color(88, 88, 88, 255);
    ctx->style->colors[MU_COLOR_BUTTONFOCUS] = mu_color(78, 78, 78, 255);
}

void count_key_stats(app_state_t *app, int *never, int *current, int *previous, int *stuck) {
    *never = *current = *previous = *stuck = 0;

    key_info_t *all_keys[]  = {app->row1, app->row2, app->row3, app->row4, app->row5, app->row6};
    int         row_sizes[] = {8, 13, 13, 13, 13, 9};

    for (int row = 0; row < 6; row++) {
        for (int i = 0; i < row_sizes[row]; i++) {
            switch (all_keys[row][i].state) {
                case KEY_STATE_NEVER_PRESSED: (*never)++; break;
                case KEY_STATE_CURRENTLY_PRESSED: (*current)++; break;
                case KEY_STATE_PREVIOUSLY_PRESSED: (*previous)++; break;
                case KEY_STATE_STUCK_DOWN: (*stuck)++; break;
            }
        }
    }
}

void render_keyboard(app_state_t *app) {
    mu_Context *ctx = app->ctx;

    if (mu_begin_window(ctx, "Keyboard Tests", mu_rect(10, 280, 700, 430))) {

        int never, current, previous, stuck;
        count_key_stats(app, &never, &current, &previous, &stuck);

        char stats[256];
        snprintf(
            stats,
            sizeof(stats),
            "Keys: Tested=%d Untested=%d Active=%d Stuck=%d",
            previous,
            never,
            current,
            stuck
        );
        mu_text(ctx, stats);
        mu_text(ctx, "Gray=Untested  Green=Active  Yellow=Tested  Red=Stuck");

        mu_layout_row(ctx, 1, (int[]){-1}, 10); // Spacer

        // Row 1: Function keys (8 keys)
        mu_layout_row(ctx, 8, (int[]){80, 80, 80, 80, 80, 80, 80, 80}, 30);
        for (int i = 0; i < 8; i++) {
            set_button_color(ctx, get_key_color(app->row1[i].state));
            mu_button(ctx, app->row1[i].label);
        }

        // Row 2: Number row (13 keys)
        mu_layout_row(ctx, 13, (int[]){50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 13; i++) {
            set_button_color(ctx, get_key_color(app->row2[i].state));
            mu_button(ctx, app->row2[i].label);
        }

        // Row 3: QWERTY row (13 keys)
        mu_layout_row(ctx, 13, (int[]){50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 13; i++) {
            set_button_color(ctx, get_key_color(app->row3[i].state));
            mu_button(ctx, app->row3[i].label);
        }

        // Row 4: ASDF row (13 keys)
        mu_layout_row(ctx, 13, (int[]){50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 13; i++) {
            set_button_color(ctx, get_key_color(app->row4[i].state));
            mu_button(ctx, app->row4[i].label);
        }

        // Row 5: ZXCV row (13 keys)
        mu_layout_row(ctx, 13, (int[]){50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 13; i++) {
            set_button_color(ctx, get_key_color(app->row5[i].state));
            mu_button(ctx, app->row5[i].label);
        }

        // Row 6: Bottom row (9 keys)
        mu_layout_row(ctx, 9, (int[]){50, 50, 50, 50, 260, 50, 50, 50, 50}, 30);
        for (int i = 0; i < 9; i++) {
            set_button_color(ctx, get_key_color(app->row6[i].state));
            mu_button(ctx, app->row6[i].label);
        }

        reset_button_colors(ctx);
        mu_end_window(ctx);
    }
}

void render_ui(app_state_t *app) {
    mu_Context *ctx = app->ctx;

    uint16_t bg_color = rgb888_to_rgb565(32, 32, 32);
    for (int i = 0; i < app->fb->w * app->fb->h; i++) {
        app->fb->pixels[i] = bg_color;
    }

    mu_begin(ctx);

    if (mu_begin_window(ctx, "WHY2025 Badge Test", mu_rect(0, 0, 720, 270))) {

        mu_layout_row(ctx, 1, (int[]){-1}, 40);
        mu_text(ctx, "WHY2025 Badge Test");

        mu_layout_row(ctx, 1, (int[]){-1}, 10);

        mu_layout_row(ctx, 2, (int[]){340, 340}, 35);

        for (int i = 0; i < app->num_tests; i++) {
            mu_Color test_color = app->tests[i].passed ? mu_color(0, 150, 0, 255) : mu_color(200, 0, 0, 255);

            set_button_color(ctx, test_color);

            char button_text[128];
            snprintf(button_text, sizeof(button_text), "%s\n%s", app->tests[i].name, app->tests[i].status);

            mu_button(ctx, button_text);

            if ((i + 1) % 2 == 0 && i < app->num_tests - 1) {
                mu_layout_row(ctx, 2, (int[]){340, 340}, 35);
            }
        }

        reset_button_colors(ctx);

        mu_layout_row(ctx, 1, (int[]){-1}, 10);

        mu_layout_row(ctx, 1, (int[]){-1}, 25);
        char input_label[300];
        snprintf(input_label, sizeof(input_label), "Keyboard Input: %s", app->input_buffer);
        mu_text(ctx, input_label);

        mu_end_window(ctx);
    }

    render_keyboard(app);

    mu_end(ctx);

    mu_Command *cmd = NULL;
    while (mu_next_command(ctx, &cmd)) {
        switch (cmd->type) {
            case MU_COMMAND_TEXT:
                draw_text(app->fb, cmd->text.str, cmd->text.pos.x, cmd->text.pos.y, cmd->text.color);
                break;
            case MU_COMMAND_RECT: draw_rect(app->fb, cmd->rect.rect, cmd->rect.color); break;
            case MU_COMMAND_ICON:
            case MU_COMMAND_CLIP: break;
        }
    }
}

int main() {
    app_state_t app = {0};

    app.window = window_create("WHY2025 Badge Test", (window_size_t){720, 720}, WINDOW_FLAG_FULLSCREEN);
    int fb_num;
    app.fb = window_framebuffer_allocate(app.window, BADGEVMS_PIXELFORMAT_RGB565, (window_size_t){720, 720}, &fb_num);

    app.ctx = malloc(sizeof(mu_Context));
    mu_init(app.ctx);
    app.ctx->text_width  = mu_text_width;
    app.ctx->text_height = mu_text_height;

    init_tests(&app);
    init_keyboard_layout(&app);
    strcpy(app.input_buffer, "Type here...");

    bool       running              = true;
    long const target_frame_time_us = 16667;

    memset(app.fb->pixels, 0x55, 720 * 720 * 2);

    render_ui(&app);
    window_framebuffer_update(app.window, fb_num, true, NULL, 0);

    while (running) {
        struct timespec start_time, cur_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        while (1) {
            clock_gettime(CLOCK_MONOTONIC, &cur_time);
            long elapsed_us =
                (cur_time.tv_sec - start_time.tv_sec) * 1000000L + (cur_time.tv_nsec - start_time.tv_nsec) / 1000L;
            long sleep_time = target_frame_time_us - elapsed_us;

            if (sleep_time <= 0) {
                break;
            }

            event_t event = window_event_poll(app.window, false, sleep_time / 1000);

            switch (event.type) {
                case EVENT_QUIT: running = false; break;

                case EVENT_KEY_DOWN:
                case EVENT_KEY_UP: handle_keyboard_event(&app, &event.keyboard); break;

                default: break;
            }

            render_ui(&app);
        }

        static uint64_t frame_counter = 0;
        frame_counter++;
        check_stuck_keys(&app, frame_counter * 16000000ULL);

        window_framebuffer_update(app.window, fb_num, true, NULL, 0);
    }

    window_destroy(app.window);
    free(app.ctx);

    return 0;
}
