/*
 * app_recorder.cpp — Voice recorder + playback (audio engine version)
 *
 * Portrait 368x448, canvas.
 *
 * Files are written to /sdcard/recordings/Recording NNNN.wav with the next
 * unused index. Length is unbounded — recording continues until BOOT is
 * pressed again. The lower half of the screen lists existing files; the
 * selected one is what PWR plays.
 *
 * Controls:
 *   BOOT short press – start / stop recording
 *   BOOT long press  – cycle selected file (down the list, wraps)
 *   PWR  short press – play / stop selected file
 */

#include "app_recorder.h"
#include "app_common.h"
#include "audio_engine.h"
#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <FS.h>
#include <math.h>
#include <string.h>
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include "TouchDrvFT6X36.hpp"

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;

#define BOOT_BTN     0
#define PWR_POLL_MS  50
#define SAMPLE_RATE  16000
#define N_MONO       512
#define REC_DIR      "/recordings"
#define BOOT_LONG_MS 800

static Arduino_Canvas  *canvas = nullptr;
static TouchDrvFT6X36   s_touch;
static bool             s_touchWas = false;

enum RecState { REC_IDLE, REC_RECORDING, REC_PLAYING, REC_PENDING };
static RecState  s_state     = REC_IDLE;
static uint32_t  s_startMs   = 0;
static bool      s_bootWas   = false;
static uint32_t  s_bootDownAt = 0;
static bool      s_bootLongFired = false;
static uint32_t  s_lastPwr   = 0;
static uint32_t  s_lastDraw  = 0;
static int16_t   s_mono[N_MONO];
static float     s_level     = 0;
static File      s_recFile;
static uint32_t  s_recBytes  = 0;
static uint32_t  s_pendEnd   = 0;

#define REC_DELAY_MS  400

// ── File list ────────────────────────────────────────────────────────────────
#define MAX_FILES   64
#define NAME_LEN    32
static char     s_files[MAX_FILES][NAME_LEN];
static int      s_nFiles  = 0;
static int      s_sel     = -1;       // index into s_files; -1 if none
static int      s_listScroll = 0;     // first visible row index

// ── Playback state ───────────────────────────────────────────────────────────
static File      s_playFile;
static int16_t   s_playLeft[N_MONO];
static int       s_playLeftN  = 0;
static int       s_playLeftPos = 0;
static char      s_playPath[64];
static char      s_recPath[64];

// ── WAV header ───────────────────────────────────────────────────────────────
static void writeWavHeader(File &f, uint32_t dataBytes) {
    uint32_t byteRate  = SAMPLE_RATE * 2;
    uint32_t chunkSize = 36 + dataBytes;
    f.write((const uint8_t *)"RIFF", 4);
    f.write((uint8_t*)&chunkSize, 4);
    f.write((const uint8_t *)"WAVE", 4);
    f.write((const uint8_t *)"fmt ", 4);
    uint32_t sc1 = 16;          f.write((uint8_t*)&sc1, 4);
    uint16_t af  = 1;           f.write((uint8_t*)&af, 2);
    uint16_t ch  = 1;           f.write((uint8_t*)&ch, 2);
    uint32_t sr  = SAMPLE_RATE; f.write((uint8_t*)&sr, 4);
    f.write((uint8_t*)&byteRate, 4);
    uint16_t ba  = 2;           f.write((uint8_t*)&ba, 2);
    uint16_t bps = 16;          f.write((uint8_t*)&bps, 2);
    f.write((const uint8_t *)"data", 4);
    f.write((uint8_t*)&dataBytes, 4);
}

// ── File helpers ─────────────────────────────────────────────────────────────
// Caller must have SD mounted. Sorted by filename ascending so the latest
// (highest numeric index) ends up last.
static void scanRecordings() {
    s_nFiles = 0;
    if (!SD_MMC.exists(REC_DIR)) {
        SD_MMC.mkdir(REC_DIR);
        return;
    }
    File dir = SD_MMC.open(REC_DIR);
    if (!dir || !dir.isDirectory()) return;
    File f;
    while ((f = dir.openNextFile()) && s_nFiles < MAX_FILES) {
        if (!f.isDirectory()) {
            const char *n = f.name();
            // Strip leading path if SD returns full path
            const char *base = strrchr(n, '/');
            if (base) base++; else base = n;
            int len = strlen(base);
            if (len > 4 && strcmp(base + len - 4, ".wav") == 0) {
                strncpy(s_files[s_nFiles], base, NAME_LEN - 1);
                s_files[s_nFiles][NAME_LEN - 1] = '\0';
                s_nFiles++;
            }
        }
        f.close();
    }
    dir.close();
    // Sort
    for (int i = 0; i < s_nFiles - 1; i++) {
        for (int j = i + 1; j < s_nFiles; j++) {
            if (strcmp(s_files[i], s_files[j]) > 0) {
                char tmp[NAME_LEN];
                strcpy(tmp, s_files[i]);
                strcpy(s_files[i], s_files[j]);
                strcpy(s_files[j], tmp);
            }
        }
    }
    if (s_nFiles == 0)              s_sel = -1;
    else if (s_sel < 0)             s_sel = s_nFiles - 1;   // newest by default
    else if (s_sel >= s_nFiles)     s_sel = s_nFiles - 1;
}

// Generate the next filename. Caller has SD mounted.
static void nextRecordingPath(char *out, size_t cap) {
    int maxIdx = 0;
    for (int i = 0; i < s_nFiles; i++) {
        // Expect "Recording NNNN.wav"
        if (strncmp(s_files[i], "Recording ", 10) == 0) {
            int n = atoi(s_files[i] + 10);
            if (n > maxIdx) maxIdx = n;
        }
    }
    snprintf(out, cap, REC_DIR "/Recording %04d.wav", maxIdx + 1);
}

// ── Drawing ──────────────────────────────────────────────────────────────────
static void draw() {
    canvas->fillScreen(0x0000);

    int16_t cx = LCD_WIDTH / 2;

    // ── State indicator pill ────────────────────────────────────────────
    const char *stStr;
    uint16_t stCol, stBg;
    if      (s_state == REC_RECORDING || s_state == REC_PENDING) { stStr = "REC"; stCol = 0xF800; stBg = 0x4000; }
    else if (s_state == REC_PLAYING)   { stStr = "PLAY";  stCol = 0x07E0; stBg = 0x0200; }
    else if (s_nFiles > 0)             { stStr = "READY"; stCol = 0xFFE0; stBg = 0x4200; }
    else                               { stStr = "IDLE";  stCol = 0x2945; stBg = 0x1082; }

    int16_t stw = (int16_t)(strlen(stStr) * 18);
    int16_t pillW = stw + 28;
    canvas->fillRoundRect(cx - pillW / 2, 48, pillW, 38, 19, stBg);
    canvas->setTextSize(3);
    canvas->setTextColor(stCol);
    canvas->setCursor(cx - stw / 2, 56);
    canvas->print(stStr);
    if (s_state == REC_RECORDING)
        canvas->fillCircle(cx - stw / 2 - 16, 68, 6, 0xF800);

    // ── Timer hero ─────────────────────────────────────────────────────
    if (s_state == REC_RECORDING || s_state == REC_PLAYING) {
        uint32_t elapsed = (millis() - s_startMs) / 1000;
        char buf[8];
        snprintf(buf, sizeof(buf), "%02lu", elapsed);
        canvas->setTextSize(6, 7, 2);
        canvas->setTextColor(s_state == REC_PLAYING ? 0x07E0 : 0xFFFF);
        int16_t tw = (int16_t)(strlen(buf) * 38);
        canvas->setCursor((LCD_WIDTH - tw) / 2, 110);
        canvas->print(buf);
        canvas->setTextSize(2);
        canvas->setTextColor(0x7BEF);
        const char *sub = (s_state == REC_PLAYING) ? "playing" : "recording";
        tw = (int16_t)(strlen(sub) * 12);
        canvas->setCursor((LCD_WIDTH - tw) / 2, 166);
        canvas->print(sub);
    } else {
        // Idle hero — file count
        char buf[24];
        snprintf(buf, sizeof(buf), "%d file%s", s_nFiles, s_nFiles == 1 ? "" : "s");
        canvas->setTextSize(3);
        canvas->setTextColor(0x528A);
        int16_t tw = (int16_t)(strlen(buf) * 18);
        canvas->setCursor((LCD_WIDTH - tw) / 2, 130);
        canvas->print(buf);
    }

    // ── Level meter (only while recording) ──────────────────────────────
    if (s_state == REC_RECORDING) {
        int16_t barX = 24, barY = 196, barW = LCD_WIDTH - 48, barH = 14;
        int meterW = (int)(s_level / 32767.0f * barW);
        if (meterW < 0) meterW = 0;
        if (meterW > barW) meterW = barW;
        canvas->fillRoundRect(barX, barY, barW, barH, 7, 0x1082);
        if (meterW > 4) {
            uint16_t mCol = (meterW > barW * 3 / 4) ? 0xF800 : 0x07E0;
            canvas->fillRoundRect(barX, barY, meterW, barH, 7, mCol);
        }
    }

    // ── File list (lower half) ─────────────────────────────────────────
    int16_t listTop = 232;
    int16_t rowH    = 30;
    int     visible = 5;            // 5 rows × 30 px = 150 px → ends at y=382

    canvas->setTextSize(1);
    canvas->setTextColor(0x528A);
    canvas->setCursor(36, listTop - 14);
    canvas->print("RECORDINGS");

    if (s_nFiles == 0) {
        canvas->setTextSize(2);
        canvas->setTextColor(0x4208);
        const char *msg = "No recordings yet";
        int16_t tw = (int16_t)(strlen(msg) * 12);
        canvas->setCursor((LCD_WIDTH - tw) / 2, listTop + 50);
        canvas->print(msg);
    } else {
        // Keep selection visible
        if (s_sel < s_listScroll)               s_listScroll = s_sel;
        if (s_sel >= s_listScroll + visible)    s_listScroll = s_sel - visible + 1;
        if (s_listScroll < 0)                   s_listScroll = 0;

        for (int vi = 0; vi < visible && (s_listScroll + vi) < s_nFiles; vi++) {
            int idx = s_listScroll + vi;
            int16_t y = listTop + vi * rowH;
            bool isSel = (idx == s_sel);

            if (isSel) {
                canvas->fillRoundRect(36, y, LCD_WIDTH - 72, rowH - 4, 6, 0x1082);
                canvas->fillRect(36, y, 4, rowH - 4, 0x07E0);
            }
            canvas->setTextSize(2);
            canvas->setTextColor(isSel ? 0xFFFF : 0x7BEF);
            canvas->setCursor(46, y + 6);
            canvas->print(s_files[idx]);
        }

        // Scroll indicators
        if (s_listScroll > 0) {
            canvas->fillTriangle(LCD_WIDTH / 2 - 5, listTop - 4,
                                 LCD_WIDTH / 2 + 5, listTop - 4,
                                 LCD_WIDTH / 2,     listTop - 10, 0x4208);
        }
        if (s_listScroll + visible < s_nFiles) {
            int16_t by = listTop + visible * rowH;
            canvas->fillTriangle(LCD_WIDTH / 2 - 5, by,
                                 LCD_WIDTH / 2 + 5, by,
                                 LCD_WIDTH / 2,     by + 6, 0x4208);
        }
    }

    // Pill labels anchored to hardware buttons
    draw_pill_label(canvas, 0, 0, "rec");
    if (s_nFiles > 0) draw_pill_label(canvas, 0, 1, "play");

    draw_battery_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_mic_pill(canvas, LCD_WIDTH, LCD_HEIGHT);
    canvas->flush();
}

// ── Recording ────────────────────────────────────────────────────────────────
static void startRecordingPending() {
    if (!SD_MMC.begin("/sdcard", true)) { USBSerial.println("SD mount failed"); return; }
    scanRecordings();
    nextRecordingPath(s_recPath, sizeof(s_recPath));
    s_recFile = SD_MMC.open(s_recPath, FILE_WRITE);
    if (!s_recFile) { USBSerial.println("Cannot open recording"); SD_MMC.end(); return; }
    writeWavHeader(s_recFile, 0);
    s_recBytes = 0;
    audio_engine_mute();
    s_state   = REC_PENDING;
    s_pendEnd = millis() + REC_DELAY_MS;
    common_activity();
}

static void startRecordingNow() {
    s_startMs = millis();
    s_state   = REC_RECORDING;
    audio_engine_record();
}

static void stopRecording() {
    audio_engine_record_stop();
    audio_engine_unmute();
    if (s_recFile) {
        s_recFile.seek(0);
        writeWavHeader(s_recFile, s_recBytes);
        s_recFile.close();
    }
    scanRecordings();
    // Select the file we just wrote (it's the newest)
    if (s_nFiles > 0) s_sel = s_nFiles - 1;
    SD_MMC.end();
    s_state = REC_IDLE;
}

// ── Playback ─────────────────────────────────────────────────────────────────
static void startPlayback() {
    if (s_sel < 0 || s_sel >= s_nFiles) return;
    if (!SD_MMC.begin("/sdcard", true)) { USBSerial.println("SD mount failed"); return; }
    snprintf(s_playPath, sizeof(s_playPath), REC_DIR "/%s", s_files[s_sel]);
    s_playFile = SD_MMC.open(s_playPath);
    if (!s_playFile) { USBSerial.println("No recording"); SD_MMC.end(); return; }
    s_playFile.seek(44);
    s_playLeftN = 0;
    s_playLeftPos = 0;
    s_startMs = millis();
    s_state   = REC_PLAYING;
    audio_engine_play();
    common_activity();
}

static void stopPlayback() {
    audio_engine_stop();
    if (s_playFile) s_playFile.close();
    SD_MMC.end();
    s_state = REC_IDLE;
}

// ── App entry points ─────────────────────────────────────────────────────────
void app_recorder_setup(Arduino_SH8601 *gfx) {
    (void)gfx;
    canvas   = g_canvas;
    s_state  = REC_IDLE;
    s_bootWas = false;
    s_bootDownAt = 0;
    s_bootLongFired = false;
    s_lastPwr = 0;
    s_lastDraw = 0;
    s_level   = 0;
    s_nFiles  = 0;
    s_sel     = -1;
    s_listScroll = 0;

    audio_engine_init();
    pinMode(BOOT_BTN, INPUT_PULLUP);
    if (!s_touch.begin(Wire, FT6X36_SLAVE_ADDRESS, IIC_SDA, IIC_SCL))
        USBSerial.println("FT6X36 init failed (recorder)");
    s_touchWas = false;

    // Initial scan
    if (SD_MMC.begin("/sdcard", true)) {
        scanRecordings();
        SD_MMC.end();
    }

    draw();
}

void app_recorder_loop() {
    common_tick();
    uint32_t now = millis();

    // ── Pending: wait for button-click delay to expire ───────────────────
    if (s_state == REC_PENDING && now >= s_pendEnd) {
        startRecordingNow();
        draw();
    }

    // ── Recording: pull mic samples, write to SD (no length limit) ───────
    if (s_state == REC_RECORDING) {
        int got = audio_engine_pull(s_mono, N_MONO);
        if (got > 0) {
            float rms = 0;
            for (int i = 0; i < got; i++)
                rms += (float)s_mono[i] * s_mono[i];
            s_level = sqrtf(rms / got);
            s_recFile.write((uint8_t*)s_mono, got * 2);
            s_recBytes += got * 2;
        }
        if (now - s_lastDraw >= 80) { s_lastDraw = now; draw(); }
    }

    // ── Playback ─────────────────────────────────────────────────────────
    if (s_state == REC_PLAYING) {
        if (s_playLeftN > 0) {
            int pushed = audio_engine_push(&s_playLeft[s_playLeftPos], s_playLeftN);
            s_playLeftPos += pushed;
            s_playLeftN   -= pushed;
        }
        if (s_playLeftN == 0) {
            int space = audio_engine_tx_space();
            if (space >= 128) {
                int toRead = (space < N_MONO) ? space : N_MONO;
                int bytes = s_playFile.read((uint8_t*)s_playLeft, toRead * 2);
                if (bytes <= 0) { stopPlayback(); draw(); return; }
                int samples = bytes / 2;
                int pushed = audio_engine_push(s_playLeft, samples);
                if (pushed < samples) {
                    s_playLeftPos = pushed;
                    s_playLeftN   = samples - pushed;
                }
            }
        }
        if (now - s_lastDraw >= 200) { s_lastDraw = now; draw(); }
    }

    // ── Touch — tap a row to select; tap scroll arrows to scroll ────────
    if (s_state == REC_IDLE && s_nFiles > 0) {
        int16_t tx, ty;
        bool touching = s_touch.getPoint(&tx, &ty, 1);
        if (touching && !s_touchWas) {
            const int16_t listTop = 232;
            const int16_t rowH    = 30;
            const int     visible = 5;
            // Up-arrow hit (above the list)
            if (tx > LCD_WIDTH/2 - 14 && tx < LCD_WIDTH/2 + 14 &&
                ty >= listTop - 16 && ty < listTop &&
                s_listScroll > 0) {
                common_activity();
                s_listScroll--;
                if (s_sel >= s_listScroll + visible) s_sel = s_listScroll + visible - 1;
                draw();
            }
            // Down-arrow hit (below the list)
            else if (tx > LCD_WIDTH/2 - 14 && tx < LCD_WIDTH/2 + 14 &&
                     ty >= listTop + visible*rowH && ty < listTop + visible*rowH + 16 &&
                     s_listScroll + visible < s_nFiles) {
                common_activity();
                s_listScroll++;
                if (s_sel < s_listScroll) s_sel = s_listScroll;
                draw();
            }
            // Row hit
            else if (tx >= 36 && tx <= LCD_WIDTH - 36 &&
                     ty >= listTop && ty < listTop + visible*rowH) {
                int row = (ty - listTop) / rowH;
                int idx = s_listScroll + row;
                if (idx >= 0 && idx < s_nFiles && idx != s_sel) {
                    common_activity();
                    s_sel = idx;
                    draw();
                }
            }
        }
        s_touchWas = touching;
    } else {
        s_touchWas = false;
    }

    // ── BOOT — short = record toggle; long = cycle file ─────────────────
    bool boot = (digitalRead(BOOT_BTN) == LOW);
    if (boot && !s_bootWas) {
        s_bootDownAt = now;
        s_bootLongFired = false;
    }
    if (boot && !s_bootLongFired && (now - s_bootDownAt) >= BOOT_LONG_MS) {
        // Long-press → cycle selected file (only meaningful when idle)
        s_bootLongFired = true;
        if (s_state == REC_IDLE && s_nFiles > 0) {
            common_activity();
            s_sel = (s_sel + 1) % s_nFiles;
            draw();
        }
    }
    if (!boot && s_bootWas) {
        // Released — fire short-press unless long already fired
        if (!s_bootLongFired) {
            common_activity();
            if      (s_state == REC_IDLE)      startRecordingPending();
            else if (s_state == REC_PENDING)   stopRecording();
            else if (s_state == REC_RECORDING) stopRecording();
            else if (s_state == REC_PLAYING)   stopPlayback();
            draw();
        }
        s_bootDownAt = 0;
    }
    s_bootWas = boot;

    // ── PWR — play / stop ───────────────────────────────────────────────
    if (common_consume_pwr_short()) {
        common_activity();
        if      (s_state == REC_IDLE && s_nFiles > 0) startPlayback();
        else if (s_state == REC_PLAYING)              stopPlayback();
        draw();
    }

    if (s_state == REC_IDLE) delay(10);
}
