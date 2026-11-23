#!/usr/bin/env python3
"""Host regression of actual UI fonts/primitives; no MCU or third-party packages.
Run: python3 /home/espdev/Documents/f411_ucc3895/tests/test_ui_render.py
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'Core/Src/ui_manager.c').read_text()
# Compile the production tables and drawing functions, without MCU dependencies.
render = source[source.index('#include "ui_font.h"'):source.index('void UI_Manager_Init')]
assert 'UI_PrintDiag(0, st_dbg);' in source
assert 'UI_PrintStr_8x16(0, 54,' not in source
status_calls = re.findall(r'UI_PrintStr_8x16\((?:4|52|96), (\d+),', source)
assert len(status_calls) == 8 and set(status_calls) == {'40'}, status_calls

prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define SSD1306_WIDTH 128U
#define SSD1306_HEIGHT 64U
static uint8_t storage[1026];
static uint8_t *OLED_Get_BackBuffer_Ptr(void) { return storage + 1; }
'''
tests = r'''
static unsigned pixel(int x, int y) {
    return (storage[1 + x + (y / 8) * 128] >> (y % 8)) & 1U;
}
static void reset(void) {
    memset(storage, 0, sizeof storage);
    storage[0] = 0xA5; storage[1025] = 0x5A;
}
static void guards(void) { assert(storage[0] == 0xA5 && storage[1025] == 0x5A); }
static void font_ink(char c, int *lo, int *hi) {
    const char *e = strchr(UI_FontChars, c);
    assert(e);
    unsigned idx = (unsigned)(e - UI_FontChars);
    *lo = 8; *hi = -1;
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 8; col++)
            if (UI_Font[idx][row] & (0x80U >> col)) {
                if (col < *lo) *lo = col;
                if (col > *hi) *hi = col;
            }
    assert(*hi >= *lo);
}
int main(void) {
    const char *missing = "0123456789ALRM!-VHOTSENF";
    for (const char *p = missing; *p; p++) {
        reset(); UI_DrawBigChar(0, 2, *p);
        unsigned lit = 0;
        for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) {
            unsigned expected = 0;
            if (x < 16 && y >= 2 && y < 34)
                expected = !!(UI_Font[strchr(UI_FontChars, *p) - UI_FontChars][(y - 2) / 2] & (0x80U >> (x / 2)));
            assert(pixel(x, y) == expected); lit += expected;
        }
        assert(lit); guards();
    }
    reset(); UI_PrintBigStr(2, "TSENSOR");
    uint8_t actual[1024]; memcpy(actual, storage + 1, sizeof actual);
    reset();
    for (int i = 0; i < 7; i++) UI_DrawBigChar(2 + 18*i, 2, "TSENSOR"[i]);
    assert(memcmp(actual, storage + 1, sizeof actual) == 0); guards();
    /* big current: byte-exact vs an independently coded layout, plus
       framebuffer geometry: glyph runs, even ink gaps, 16x32 unit on the
       same top/bottom lines as the digits. */
    const int gapD = 4, gapU = 8;
    for (int current = 0; current <= 999; current++) {
        reset(); UI_PrintCurrent(current);
        memcpy(actual, storage + 1, sizeof actual);
        char text[5]; snprintf(text, sizeof text, "%d", current);
        int n = (int)strlen(text);
        int glo[3], ghi[3], alo, ahi;
        font_ink('A', &alo, &ahi);
        for (int i = 0; i < n; i++) font_ink(text[i], &glo[i], &ghi[i]);
        int xs[4], ink = 0;
        for (int i = 0; i < n; i++) {
            xs[i] = ink - glo[i] * 3;
            ink += (ghi[i] - glo[i] + 1) * 3 + gapD;
        }
        int dwidth = ink - gapD;
        int total = dwidth + gapU + (ahi - alo + 1) * 2;
        int x0 = (128 - total) / 2;
        reset();
        for (int i = 0; i < n; i++) UI_DrawScaledChar(x0 + xs[i], 2, text[i], 3, 2);
        UI_DrawScaledChar(x0 + dwidth + gapU - alo * 2, 2, 'A', 2, 2);
        assert(memcmp(actual, storage + 1, sizeof actual) == 0);
        for (int y = 0; y < 64; y++) {
            if (y >= 2 && y < 34) continue;
            for (int x = 0; x < 128; x++) assert(!pixel(x, y));
        }
        int segs[8][2], ns = 0;
        for (int x = 0; x < 128; x++) {
            int lit = 0;
            for (int y = 2; y < 34; y++) lit |= pixel(x, y);
            if (!lit) continue;
            if (ns && segs[ns - 1][1] == x - 1) segs[ns - 1][1] = x;
            else { assert(ns < 8); segs[ns][0] = segs[ns][1] = x; ns++; }
        }
        assert(ns == n + 1);
        assert(segs[0][0] == x0);
        for (int i = 0; i < ns; i++) {
            int lo = i < n ? glo[i] : alo, hi = i < n ? ghi[i] : ahi;
            assert(segs[i][1] - segs[i][0] + 1 == (hi - lo + 1) * (i < n ? 3 : 2));
            int ymin = 64, ymax = -1;
            for (int y = 2; y < 34; y++)
                for (int x = segs[i][0]; x <= segs[i][1]; x++)
                    if (pixel(x, y)) {
                        if (y < ymin) ymin = y;
                        if (y > ymax) ymax = y;
                    }
            assert(ymin == 2 && ymax == 33);
            if (i) assert(segs[i][0] - segs[i - 1][1] - 1 == (i == n ? gapU : gapD));
        }
        guards();
    }
    reset(); UI_PrintCurrent(-1); memcpy(actual, storage + 1, sizeof actual);
    reset(); UI_PrintCurrent(0); assert(!memcmp(actual, storage + 1, sizeof actual));
    reset(); UI_PrintCurrent(1000); memcpy(actual, storage + 1, sizeof actual);
    reset(); UI_PrintCurrent(999); assert(!memcmp(actual, storage + 1, sizeof actual));
    const char *diag[] = {"V15 69:5", "D000", "A2048", "B2048", "F2048", "E4095", "M99999"};
    for (unsigned i = 0; i < sizeof diag / sizeof *diag; i++) {
        reset(); UI_PrintStr_8x16(4, 40, "100A", 1);
        memcpy(actual, storage + 1, sizeof actual);
        UI_PrintDiag(0, diag[i]);
        assert(memcmp(actual, storage + 1, 896) == 0); // rows 0..55 untouched
        for (int y = 56; y < 64; y++) for (int x = 0; x < 128; x++) {
            unsigned expected = 0;
            if ((unsigned)(x / 8) < strlen(diag[i])) {
                unsigned c = (unsigned char)diag[i][x / 8] - 32U;
                unsigned r = (unsigned)(y - 56) * 2;
                expected = !!((Font_8x16[c][r] | Font_8x16[c][r+1]) & (0x80U >> (x % 8)));
            }
            assert(pixel(x, y) == expected);
        }
        guards();
    }
    reset(); UI_DrawHorizontalLine(0, 127, 37, 1);
    for (int x = 0; x < 128; x++) assert(pixel(x, 37));
    guards();
    puts("PASS: font glyphs, TSENSOR width, even big-digit gaps, 16x32 unit, "
         "1000 current values, clamps, diagnostic phases, status isolation, guards");
}
'''
with tempfile.TemporaryDirectory(prefix='ui-render-') as tmp:
    tmp = Path(tmp)
    c = tmp / 'render.c'
    c.write_text(prefix + render + tests)
    exe = tmp / 'render'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-I', str(ROOT / 'Core/Inc'), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
