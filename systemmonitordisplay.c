/* systemmonitordisplay.c - a green-screen terminal sensor panel for a small
 * secondary monitor, fed by HWiNFO64 (free or Pro).
 *
 *   systemmonitordisplay.exe                    the panel
 *   systemmonitordisplay.exe --diagnose         monitors and HWiNFO readings
 *   systemmonitordisplay.exe --preview out.bmp [cpu temp gpu] [--alarm]
 *   systemmonitordisplay.exe --selftest
 *   systemmonitordisplay.exe --check-translations
 *   systemmonitordisplay.exe --version
 *
 * Settings live in systemmonitordisplay.json beside the exe, or in
 * %APPDATA%\System Monitor Display if the exe's folder cannot be written
 * to. It is written with the defaults on first run and re-read whenever
 * it is saved. Faults go to systemmonitordisplay.log beside it.
 *
 * The file runs from what depends on nothing to what depends on it all:
 * helpers, settings, HWiNFO, readings, drawing, the state, the checks,
 * and then the Windows side: translations, screens and tray, the settings
 * window, the panel window, and the start. Everything above "Windows" is
 * plain C and builds anywhere.
 *
 * Build: build.cmd (MinGW-w64 or MSVC) or build.sh (MinGW-w64 on Linux).
 * It links only Windows' own libraries: user32, gdi32, shell32, advapi32,
 * comctl32 (the settings window's controls), comdlg32 (the colour picker),
 * ole32 (the Startup shortcut), uxtheme (the ripple gap's slider) and winmm
 * (the degauss's sound). app.rc adds the icon, and the manifest that gives
 * the settings window today's Windows controls.
 * The drawing and logic also build on Linux (gcc ... -lm) for --selftest
 * and --preview; the panel itself needs Windows.
 */
#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L   /* clock_gettime */
#endif
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <uxtheme.h>
#include <mmsystem.h>
#include <vsstyle.h>
#else
#include <sys/stat.h>
#endif
#include "font8x16.h"

#define NAME "System Monitor Display"
#define VERSION "1.4.0"
#define HOME_PAGE "https://github.com/JaredCabot/system-monitor-display"
#define WIDE_(s) L##s
#define WIDE(s) WIDE_(s)
#define VERSION_W WIDE(VERSION)
#define SLUG "systemmonitordisplay"
#define PANEL_CLASS L"SystemMonitorDisplay"           /* window classes */
#define SETTINGS_CLASS L"SystemMonitorDisplaySettings"
#define RANGE_CLASS L"SystemMonitorDisplayRange"

/* ------------------------------------------------------------ helpers */
static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}

static double uniform(double lo, double hi)
{
    return lo + (hi - lo) * (rand() / (double)RAND_MAX);
}

static void log_note(const char *fmt, ...);

static void *must_alloc(size_t bytes)
{
    void *p = calloc(1, bytes);
    if (!p) {
        log_note("out of memory (%lu bytes); stopped", (unsigned long)bytes);
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

/* Files beside the program, so copying the exe takes its settings along;
   or, if Windows will not let the program write in its own folder (under
   Program Files, say), in %APPDATA%\System Monitor Display. */
#ifdef _WIN32
#define LONG_PATH 4096          /* room for folders deeper than Windows' usual 260 */

/* path in the \\?\ form if it is 260 characters or more, which Windows
   needs to open it. */
static void long_form(const wchar_t *path, wchar_t *out, size_t len)
{
    out[0] = 0;
    if (wcslen(path) >= MAX_PATH - 1 && wcsncmp(path, L"\\\\?\\", 4)) {
        if (path[0] == L'\\' && path[1] == L'\\')            /* \\server\share */
            wcscpy(out, L"\\\\?\\UNC"), wcsncat(out, path + 1, len - 9);
        else
            wcscpy(out, L"\\\\?\\"), wcsncat(out, path, len - 5);
    } else
        wcsncat(out, path, len - 1);
}

/* The folder the settings, presets and log go in, ending in \. Decided
   once: the program's folder if a file can be made there, otherwise
   %APPDATA%\System Monitor Display, made if need be. */
static const wchar_t *data_folder(void)
{
    static wchar_t dir[LONG_PATH];
    wchar_t exe[LONG_PATH], test[LONG_PATH], *slash;
    HANDLE h;
    DWORD n;
    if (dir[0])
        return dir;
    n = GetModuleFileNameW(NULL, exe, LONG_PATH);
    if (n == 0 || n >= LONG_PATH)
        exe[0] = 0;
    slash = wcsrchr(exe, L'\\');
    if (slash)
        slash[1] = 0;
    wcscpy(dir, exe);
    _snwprintf(test, LONG_PATH, L"%ls%ls", exe, L"" SLUG L".write-test");
    test[LONG_PATH - 1] = 0;
    long_form(test, exe, LONG_PATH);       /* exe: reused for the test's long form */
    h = CreateFileW(exe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);                     /* writable: gone again on closing */
    else if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0,
                                        exe))) {
        _snwprintf(dir, LONG_PATH, L"%ls\\%ls\\", exe, L"" NAME);
        dir[LONG_PATH - 1] = 0;
        long_form(dir, test, LONG_PATH);
        CreateDirectoryW(test, NULL);
    }
    return dir;
}

/* name (UTF-8) in the data folder, in the long form if need be. */
static void beside(const char *name, wchar_t *out, size_t len)
{
    wchar_t path[LONG_PATH];
    size_t k = wcslen(data_folder());
    wcscpy(path, data_folder());
    MultiByteToWideChar(CP_UTF8, 0, name, -1, path + k, (int)(LONG_PATH - k));
    long_form(path, out, len);
}

static FILE *open_beside(const char *name, const char *mode)
{
    wchar_t path[LONG_PATH], wmode[8];
    beside(name, path, LONG_PATH);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8);
    return _wfopen(path, wmode);
}

static uint64_t mtime_beside(const char *name)
{
    wchar_t path[LONG_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    beside(name, path, LONG_PATH);
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a))
        return 0;
    return ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32)
           | a.ftLastWriteTime.dwLowDateTime;
}
#else
static FILE *open_beside(const char *name, const char *mode)
{
    return fopen(name, mode);
}

static uint64_t mtime_beside(const char *name)
{
    struct stat s;
    return stat(name, &s) ? 0 : (uint64_t)s.st_mtime;
}
#endif

/* Faults only: a session with nothing to report writes nothing. */
static void log_note(const char *fmt, ...)
{
    FILE *f = open_beside(SLUG ".log", "a");
    time_t t = time(NULL);
    char when[32];
    va_list ap;
    if (!f)
        return;
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "%s ", when);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ----------------------------------------------------------- settings */
#define DEG "\xC2\xB0"          /* the degree sign, UTF-8 as in the file */
#define TITLE_MAX 14            /* characters: fits a box with the brackets */
#define UNIT_MAX 8
#define HEADER_MAX 52           /* characters: a header line across the screen */

typedef struct {
    char title[48], reading[288], unit[24];  /* UTF-8; 14 and 8 characters */
    double decimals, bar_min, bar_max, alarm_on, alarm_off, blink_at, hold_s;
} BoxSet;

typedef struct {
    BoxSet box[3];              /* left to right */
    double smoothing_s, stale_after_s;
    double header;
    char header1[128], header2[128], phosphor[16];
    double curvature, glow, brightness, contrast, scanline_px, fps;
    char scan_dir[8];           /* "up" or "down", as hum_dir and ripple_dir */
    double scan_speed;
    double hum_bar, hum_period, hum_intensity, hum_thickness;
    char hum_dir[8];
    double ripple;
    char ripple_dir[8];
    double ripple_amplitude, ripple_frequency, ripple_thickness, ripple_drift, ripple_period;
    double ripple_static;
    double ripple_min, ripple_max;
    double afterglow_s;
    double visible_top, visible_bottom;
    char language[16];          /* "auto" for Windows' own, or a catalogue's tag */
} Settings;

enum { T_NUM, T_INT, T_BOOL, T_STR };
typedef struct {
    const char *key;
    int type;
    size_t off;
    double lo, hi, def;
    const char *sdef;
    size_t len;
} Field;

#define NUMF(k, f, lo, hi, d) { k, T_NUM, offsetof(Settings, f), lo, hi, d, 0, 0 }
#define INTF(k, f, lo, hi, d) { k, T_INT, offsetof(Settings, f), lo, hi, d, 0, 0 }
#define BOOLF(k, f, d) { k, T_BOOL, offsetof(Settings, f), 0, 1, d, 0, 0 }
#define STRF(k, f, d) \
    { k, T_STR, offsetof(Settings, f), 0, 0, 0, d, sizeof(((Settings *)0)->f) }
#define BIG 1e6
#define BOXF(n, t_, r_, u_, lo, hi, on, off, blink, hold)                    \
    STRF("box" #n "_title", box[n - 1].title, t_),                            \
    STRF("box" #n "_reading", box[n - 1].reading, r_),                        \
    STRF("box" #n "_unit", box[n - 1].unit, u_),                              \
    INTF("box" #n "_decimals", box[n - 1].decimals, 0, 2, 0),                 \
    NUMF("box" #n "_bar_min", box[n - 1].bar_min, -BIG, BIG, lo),             \
    NUMF("box" #n "_bar_max", box[n - 1].bar_max, -BIG, BIG, hi),             \
    NUMF("box" #n "_alarm_on", box[n - 1].alarm_on, -BIG, BIG, on),           \
    NUMF("box" #n "_alarm_off", box[n - 1].alarm_off, -BIG, BIG, off),        \
    NUMF("box" #n "_blink_at", box[n - 1].blink_at, -BIG, BIG, blink),        \
    NUMF("box" #n "_alarm_hold_s", box[n - 1].hold_s, 0, 600, hold)

/* In the order they are written to a new settings file. */
static const Field FIELDS[] = {
    BOXF(1, "CPU LOAD", "Total CPU Usage", "%", 0, 100, 95, 90, 100, 10),
    BOXF(2, "CPU TEMP", "CPU (Tctl/Tdie)", DEG "C", 20, 100, 90, 85, 95, 0),
    BOXF(3, "GPU LOAD", "GPU Core Load", "%", 0, 100, 95, 90, 100, 10),
    NUMF("smoothing_s", smoothing_s, 0, 30, 3),
    NUMF("stale_after_s", stale_after_s, 2, 600, 10),
    BOOLF("header", header, 0),
    STRF("header_line1", header1, ""),
    STRF("header_line2", header2, ""),
    STRF("phosphor", phosphor, "fallout"),
    NUMF("curvature", curvature, -0.15, 0.15, 0.04),
    NUMF("glow", glow, 0, 2, 1),
    NUMF("brightness", brightness, -1, 1, 0),
    NUMF("contrast", contrast, 0.5, 2, 1),
    INTF("scanline_px", scanline_px, 1, 8, 4),
    STRF("scanline_drift_direction", scan_dir, "up"),
    NUMF("scanline_drift_speed", scan_speed, 0, 30, 0),
    INTF("fps", fps, 0, 1000, 30),
    BOOLF("hum_bar", hum_bar, 1),
    STRF("hum_bar_direction", hum_dir, "up"),
    NUMF("hum_bar_period_s", hum_period, 1, 600, 16.7),
    NUMF("hum_bar_intensity", hum_intensity, 0, 10, 3),
    INTF("hum_bar_thickness", hum_thickness, 8, 400, 200),
    BOOLF("ripple", ripple, 1),
    STRF("ripple_direction", ripple_dir, "up"),
    NUMF("ripple_amplitude_pct", ripple_amplitude, 0, 10, 3),
    NUMF("ripple_frequency_hz", ripple_frequency, 10, 3000, 30),
    NUMF("ripple_thickness", ripple_thickness, 10, 300, 40),
    NUMF("ripple_drift_hz", ripple_drift, -6, 6, 0),
    NUMF("ripple_period_s", ripple_period, 0.1, 60, 23.3),
    NUMF("ripple_static", ripple_static, 0, 1, 0.3),
    NUMF("ripple_gap_min_s", ripple_min, 5, 60, 20),
    NUMF("ripple_gap_max_s", ripple_max, 5, 60, 60),
    NUMF("afterglow_s", afterglow_s, 0, 4, 0.15),
    INTF("visible_top", visible_top, 0, 160, 80),
    INTF("visible_bottom", visible_bottom, 0, 160, 0),
    STRF("language", language, "auto"),
};
#define NFIELDS (sizeof FIELDS / sizeof FIELDS[0])

/* The bytes a field's value takes in Settings. */
static size_t field_size(const Field *f)
{
    return f->type == T_STR ? f->len : sizeof(double);
}

/* The presets: text, background, centre haze (sRGB). */
static const struct {
    const char *name;
    unsigned char c[3][3];
} PHOSPHORS[] = {
    { "fallout", { { 0x1A, 0xFF, 0x80 }, { 0x01, 0x0D, 0x06 }, { 0x05, 0x30, 0x1A } } },
    { "green", { { 0x4B, 0xFF, 0x7B }, { 0x04, 0x10, 0x0A }, { 0x0B, 0x26, 0x15 } } },
    { "amber", { { 0xFF, 0xB2, 0x33 }, { 0x12, 0x0A, 0x02 }, { 0x2A, 0x1A, 0x06 } } },
    { "white", { { 0xDC, 0xE8, 0xF2 }, { 0x07, 0x0A, 0x0E }, { 0x16, 0x1C, 0x24 } } },
    { "blue", { { 0x4D, 0xA6, 0xFF }, { 0x04, 0x08, 0x0D }, { 0x0F, 0x20, 0x30 } } },
};
#define NPHOSPHORS (sizeof PHOSPHORS / sizeof PHOSPHORS[0])

static int hex_colour(const char *s, unsigned char rgb[3])
{
    unsigned v;
    char end;
    if (s[0] != '#' || strlen(s) != 7 || sscanf(s + 1, "%6x%c", &v, &end) != 1)
        return 0;
    rgb[0] = (unsigned char)(v >> 16), rgb[1] = (unsigned char)(v >> 8);
    rgb[2] = (unsigned char)v;
    return 1;
}

/* The phosphor's text, background and haze colours (sRGB). A preset by
   name, or "#RRGGBB": then the background and haze are that colour at 5 %
   and 19 %, which is what the presets work out to. Returns 0 if the name
   is neither. */
static int phosphor_colours(const char *name, unsigned char out[3][3])
{
    unsigned char rgb[3];
    size_t i;
    int c;
    for (i = 0; i < NPHOSPHORS; i++)
        if (!strcmp(PHOSPHORS[i].name, name)) {
            memcpy(out, PHOSPHORS[i].c, 9);
            return 1;
        }
    if (!hex_colour(name, rgb))
        return 0;
    for (c = 0; c < 3; c++) {
        out[0][c] = rgb[c];
        out[1][c] = (unsigned char)lround(rgb[c] * 0.05);
        out[2][c] = (unsigned char)lround(rgb[c] * 0.19);
    }
    return 1;
}

/* A minimal reader for the flat JSON objects the settings and language
   files hold. Read as if a person typed it: it stops quietly at the first
   thing it cannot read. */
typedef struct {
    const char *p;
} Json;

static void json_ws(Json *j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')
        j->p++;
}

/* Four hex digits of a \u escape; -1 if they are not. */
static long json_hex4(Json *j)
{
    long v = 0;
    int k;
    for (k = 0; k < 4; k++, j->p++) {
        int c = (unsigned char)*j->p;
        if (!isxdigit(c))
            return -1;
        v = v * 16 + (isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
    }
    return v;
}

static int json_string(Json *j, char *out, size_t len)
{
    size_t n = 0;
    if (*j->p != '"')
        return 0;
    j->p++;
    while (*j->p && *j->p != '"') {
        unsigned char bytes[4];
        int count = 1, k;
        char c = *j->p++;
        bytes[0] = (unsigned char)c;
        if (c == '\\') {
            c = *j->p++;
            bytes[0] = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r'
                     : c == 'b' ? '\b' : c == 'f' ? '\f' : (unsigned char)c;
            if (!c)
                return 0;
            if (c == 'u') {                  /* \u00B0 and the like, to UTF-8 */
                long v = json_hex4(j);
                if (v >= 0xD800 && v < 0xDC00 && j->p[0] == '\\' && j->p[1] == 'u') {
                    long lo;
                    j->p += 2;
                    lo = json_hex4(j);
                    v = lo >= 0xDC00 && lo < 0xE000 ? 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00)
                                                    : -1;
                }
                if (v < 0 || (v >= 0xD800 && v < 0xE000))
                    v = '?';
                if (v < 0x80)
                    bytes[0] = (unsigned char)v;
                else if (v < 0x800)
                    count = 2, bytes[0] = (unsigned char)(0xC0 | v >> 6);
                else if (v < 0x10000)
                    count = 3, bytes[0] = (unsigned char)(0xE0 | v >> 12);
                else
                    count = 4, bytes[0] = (unsigned char)(0xF0 | v >> 18);
                for (k = 1; k < count; k++)
                    bytes[k] = (unsigned char)(0x80 | ((v >> (6 * (count - 1 - k))) & 0x3F));
            }
        }
        if (n + count < len)                 /* whole characters only */
            for (k = 0; k < count; k++)
                out[n++] = (char)bytes[k];
    }
    if (*j->p != '"')
        return 0;
    j->p++;
    out[n] = 0;
    return 1;
}

/* The start of an object: past a BOM (Notepad adds one) and the brace.
   0 if the text is not an object. */
static int json_open(Json *j, const char *text)
{
    j->p = text;
    if (!strncmp(j->p, "\xEF\xBB\xBF", 3))
        j->p += 3;
    json_ws(j);
    return *j->p++ == '{';
}

/* The next key of the object j is in, leaving j at its value. 0 at the
   object's end (j is left at the brace), or at anything it cannot read. */
static int json_key(Json *j, char *key, size_t len)
{
    json_ws(j);
    if (*j->p == ',')
        j->p++, json_ws(j);
    if (!json_string(j, key, len))
        return 0;
    json_ws(j);
    if (*j->p++ != ':')
        return 0;
    json_ws(j);
    return 1;
}

static int settings_fields_find(const char *key)
{
    size_t i;
    for (i = 0; i < NFIELDS; i++)
        if (!strcmp(FIELDS[i].key, key))
            return (int)i;
    return -1;
}

static void settings_defaults(Settings *s)
{
    size_t i;
    memset(s, 0, sizeof *s);
    for (i = 0; i < NFIELDS; i++) {
        const Field *f = &FIELDS[i];
        if (f->type == T_STR)
            snprintf((char *)s + f->off, f->len, "%s", f->sdef);
        else
            *(double *)((char *)s + f->off) = f->def;
    }
}

/* Fields first to end - 1 as a JSON object. */
static void settings_write_part(FILE *out, const Settings *s, size_t first, size_t end)
{
    size_t i;
    fprintf(out, "{\n");
    for (i = first; i < end; i++) {
        const Field *f = &FIELDS[i];
        const char *at = (const char *)s + f->off;
        fprintf(out, "  \"%s\": ", f->key);
        if (f->type == T_STR) {
            const char *c;
            fputc('"', out);
            for (c = at; *c; c++) {
                if (*c == '"' || *c == '\\')
                    fputc('\\', out);
                fputc(*c, out);
            }
            fputc('"', out);
        } else if (f->type == T_BOOL)
            fprintf(out, "%s", *(const double *)at ? "true" : "false");
        else if (f->type == T_INT)
            fprintf(out, "%d", (int)*(const double *)at);
        else
            fprintf(out, "%.10g", *(const double *)at);
        fprintf(out, "%s\n", i + 1 < end ? "," : "");
    }
    fprintf(out, "}\n");
}

/* Fields first to end - 1 into a file beside the program, written as a
   temporary file and then swapped in, so a crash or power cut part-way
   leaves the old file whole. Returns 0 if it could not be written. */
static int write_beside(const char *name, const Settings *s, size_t first, size_t end)
{
    char tmp[300];
    FILE *f;
    int ok;
    snprintf(tmp, sizeof tmp, "%s.tmp", name);
    if (!(f = open_beside(tmp, "w")))
        return 0;
    settings_write_part(f, s, first, end);
    ok = fflush(f) == 0 && !ferror(f);
    ok &= fclose(f) == 0;
#ifdef _WIN32
    {
        static wchar_t from[LONG_PATH], to[LONG_PATH];
        beside(tmp, from, LONG_PATH);
        beside(name, to, LONG_PATH);
        ok = ok && MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok)
            DeleteFileW(from);
    }
#else
    ok = ok && rename(tmp, name) == 0;
#endif
    return ok;
}

/* 1.3.0 kept the hum bar's and ripple's speeds in pixels a second and the
   ripple's strength as a multiple of 2 pixels, and had the hum bar's
   thickness and the ripple's thickness and frequency built in. A file or
   preset of then is read as the same picture now: settings_from_old turns
   its speeds and strength into today's times and percentage, and gives
   what was built in the values it had, unless the file has today's
   settings as well. */
enum { OLD_HUM_SPEED, OLD_RIPPLE_SPEED, OLD_RIPPLE_INTENSITY, NOLD };
static const char *const OLD_KEYS[NOLD] = { "hum_bar_speed", "ripple_speed", "ripple_intensity" };
enum { NEW_HUM_PERIOD, NEW_RIPPLE_PERIOD, NEW_RIPPLE_AMPLITUDE,     /* in OLD_KEYS' order */
       NEW_HUM_THICKNESS, NEW_RIPPLE_THICKNESS, NEW_RIPPLE_FREQUENCY, NNEW };
static const char *const NEW_KEYS[NNEW] = { "hum_bar_period_s", "ripple_period_s",
                                            "ripple_amplitude_pct", "hum_bar_thickness",
                                            "ripple_thickness", "ripple_frequency_hz" };
typedef struct {
    double old[NOLD];           /* 1.3.0's values, where got */
    int got[NOLD], have[NNEW];  /* have: the file holds today's setting */
} OldSettings;
static void settings_from_old(Settings *s, const OldSettings *o);

/* Parse settings text over the defaults. Read as if a person typed it: a
   value of the wrong kind keeps its default, a number is held to its
   range, and parsing stops quietly at the first thing it cannot read. */
static void settings_parse(Settings *s, const char *text)
{
    Json j;
    char key[64], str[320];
    OldSettings o;
    int i;
    memset(&o, 0, sizeof o);
    if (!json_open(&j, text))
        return;
    while (json_key(&j, key, sizeof key)) {
        int fi = settings_fields_find(key);
        for (i = 0; i < NNEW; i++)
            o.have[i] |= !strcmp(key, NEW_KEYS[i]);
        for (i = 0; i < NOLD; i++)
            if (!strcmp(key, OLD_KEYS[i])) {    /* a number: a speed above 0, a strength from 0 */
                char *end;
                double v = strtod(j.p, &end);
                if (end != j.p && (i == OLD_RIPPLE_INTENSITY ? v >= 0 : v > 0))
                    o.old[i] = v, o.got[i] = 1;
            }
        if (*j.p == '"') {
            if (!json_string(&j, str, sizeof str))
                break;
            if (fi >= 0 && FIELDS[fi].type == T_STR)
                snprintf((char *)s + FIELDS[fi].off, FIELDS[fi].len, "%s", str);
            else if (fi >= 0 && FIELDS[fi].type != T_BOOL) {
                char *end;
                double v = strtod(str, &end);
                if (end != str && !*end)
                    *(double *)((char *)s + FIELDS[fi].off) = v;
            }
        } else if (!strncmp(j.p, "true", 4) || !strncmp(j.p, "false", 5)) {
            int v = *j.p == 't';
            j.p += v ? 4 : 5;
            if (fi >= 0 && FIELDS[fi].type == T_BOOL)
                *(double *)((char *)s + FIELDS[fi].off) = v;
        } else if (!strncmp(j.p, "null", 4)) {
            j.p += 4;
        } else {
            char *end;
            double v = strtod(j.p, &end);
            if (end == j.p)
                break;
            j.p = end;
            if (fi >= 0 && (FIELDS[fi].type == T_NUM || FIELDS[fi].type == T_INT))
                *(double *)((char *)s + FIELDS[fi].off) = v;
        }
    }
    settings_from_old(s, &o);
}

static const int BOX_TOPS[3] = { 29, 61, 93 };   /* by header lines */
typedef char HeaderLines[2][128];   /* the header as shown; room for a setting's 127 bytes */
static int header_lines(const Settings *s, HeaderLines lines);

/* Cut s to its first `chars` characters (UTF-8), clearing what follows. */
static void cut_chars(char *s, size_t size, int chars)
{
    int n = 0, count = 0;
    while (s[n] && n < (int)size - 1) {
        if (((unsigned char)s[n] & 0xC0) != 0x80 && ++count > chars)
            break;
        n++;
    }
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;                                /* never half a character */
    memset(s + n, 0, size - n);
}

/* The hum bar's, the ripple's and the scanlines' "up" or "down". */
static int is_direction(const char *key)
{
    size_t n = strlen(key);
    return n > 10 && !strcmp(key + n - 10, "_direction");
}

/* A language's tag, such as "de" or "zh-Hans": letters, digits and
   hyphens only, as it goes into a file name. */
static int tag_ok(const char *tag)
{
    return *tag && strlen(tag) < 16
           && strspn(tag, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")
                  == strlen(tag);
}

/* Every setting made one the program can use: numbers in range, names
   that mean something, and each string's unused room cleared, so two
   settings that read the same compare the same. */
static void settings_tidy(Settings *s)
{
    unsigned char ph[3][3];
    size_t i;
    int b;
    for (i = 0; i < NFIELDS; i++) {
        const Field *f = &FIELDS[i];
        char *str = (char *)s + f->off;
        double *v = (double *)str;
        if (f->type == T_STR) {             /* a direction is up or down */
            if (is_direction(f->key) && strcmp(str, "up") && strcmp(str, "down"))
                snprintf(str, f->len, "%s", f->sdef);
            continue;
        }
        if (!(*v == *v))                        /* NaN */
            *v = f->def;
        if (f->type == T_INT)
            *v = floor(*v);
        if (f->type != T_BOOL)
            *v = *v < f->lo ? f->lo : *v > f->hi ? f->hi : *v;
    }
    if (!phosphor_colours(s->phosphor, ph))
        snprintf(s->phosphor, sizeof s->phosphor, "%s", "fallout");
    if (!tag_ok(s->language))                /* "auto" or a tag, never a path */
        snprintf(s->language, sizeof s->language, "%s", "auto");
    for (b = 0; b < 3; b++) {
        BoxSet *x = &s->box[b];
        cut_chars(x->title, sizeof x->title, TITLE_MAX);
        cut_chars(x->unit, sizeof x->unit, UNIT_MAX);
        if (x->bar_max == x->bar_min)
            x->bar_max = x->bar_min + 1;
    }
    s->fps = s->fps < 23 ? 15 : 30;
    if (s->ripple_min > s->ripple_max) {
        double t = s->ripple_min;
        s->ripple_min = s->ripple_max;
        s->ripple_max = t;
    }
    for (i = 0; i < NFIELDS; i++)            /* last, after the corrections above */
        if (FIELDS[i].type == T_STR) {
            char *str = (char *)s + FIELDS[i].off;
            size_t n = strnlen(str, FIELDS[i].len - 1);
            memset(str + n, 0, FIELDS[i].len - n);
        }
}

/* The most rows the display's two hidden strips can come to: 160, and
   fewer with a header, so the numbers keep at least 9 rows (their smallest
   size) between title and bar. */
static int hidden_cap(const Settings *s)
{
    HeaderLines lines;
    int cap = 250 - BOX_TOPS[header_lines(s, lines)];
    return cap < 160 ? cap : 160;
}

/* The hidden rows in use, held to that. Worked out when drawing, so the
   settings keep what was set. */
static void hidden_rows(const Settings *s, int *top, int *bottom)
{
    int cap = hidden_cap(s);
    *top = (int)s->visible_top < cap ? (int)s->visible_top : cap;
    *bottom = (int)s->visible_bottom < cap - *top ? (int)s->visible_bottom : cap - *top;
}

static char *read_all(FILE *f)
{
    long n;
    char *buf;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1 << 20)
        return NULL;
    buf = must_alloc((size_t)n + 1);
    n = (long)fread(buf, 1, (size_t)n, f);
    buf[n] = 0;
    return buf;
}

/* The settings. Never fails: a missing file is written with the defaults,
   so there is one to edit. */
static void settings_load(Settings *s, const char *name)
{
    FILE *f = open_beside(name, "rb");
    settings_defaults(s);
    if (!f)
        write_beside(name, s, 0, NFIELDS);
    else {
        char *text = read_all(f);
        fclose(f);
        if (text) {
            settings_parse(s, text);
            free(text);
        }
    }
    settings_tidy(s);
}

/* ------------------------------------------------------------- HWiNFO */
/* One HWiNFO reading: its sensor's name and its own label, each as the
   user renamed it ([0]) and as HWiNFO named it ([1]). */
typedef struct {
    const char *sensor[2], *label[2];
    double value;
} Reading;

typedef void (*Visit)(void *ctx, const Reading *r);

/* A box's reading is a label ("GPU Core Load"), or a sensor and a label
   ("dGPU [#0]: NVIDIA GeForce RTX 4070 Ti | GPU Core Load") when the label
   alone is on more than one sensor. Either name of each will do. */
#define SENSOR_SEP " | "

static int reading_matches(const char *want, const Reading *r)
{
    const char *sep = strstr(want, SENSOR_SEP), *label = sep ? sep + 3 : want;
    int i, ok = 0;
    for (i = 0; i < 2; i++)
        ok |= !strcmp(label, r->label[i]);
    if (!ok || !sep)
        return ok;
    for (i = 0; i < 2; i++)
        if (strlen(r->sensor[i]) == (size_t)(sep - want)
            && !strncmp(want, r->sensor[i], (size_t)(sep - want)))
            return 1;
    return 0;
}

typedef struct {
    const char *labels[3];
    double value[3];
    int have[3];
} Want;

static void want_visit(void *ctx, const Reading *r)
{
    Want *w = ctx;
    int i;
    for (i = 0; i < 3; i++)
        if (!w->have[i] && reading_matches(w->labels[i], r)) {
            w->value[i] = r->value;
            w->have[i] = 1;
        }
}

#ifdef _WIN32
/* Every reading in HWiNFO's shared memory. Layout from the HWiNFO SDK:
   a header (the fields read here are in its first 44 bytes; current
   versions make it 48), then the sensor and reading elements. In a sensor,
   the original name at 8 and the user's at 136; in a reading, its
   sensor's index at 4, the original label at 12, the user's at 140 (all
   128 bytes, in the Windows code page), the value (double) at 284.
   Newer versions add the user's names in UTF-8, which the settings use:
   a sensor's at 264 (elements of 392 bytes or more) and a reading's at
   316 (444 or more). Opened and closed on every read,
   so nothing goes stale when HWiNFO restarts or the free edition switches
   it off. Returns 0 when it is not there. */
static int shm_each(Visit visit, void *ctx, uint64_t *stamp)
{
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
    const unsigned char *v;
    MEMORY_BASIC_INFORMATION mbi;
    uint32_t s_off, s_size, s_count, offset, size, count, i, at;
    int ok = 0;
    if (!h)
        return 0;
    v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (v && VirtualQuery(v, &mbi, sizeof mbi) && mbi.RegionSize >= 44
        && !memcmp(v, "HWiS", 4)) {            /* "DAED" once HWiNFO stops */
        memcpy(stamp, v + 12, 8);
        memcpy(&s_off, v + 20, 4);
        memcpy(&s_size, v + 24, 4);
        memcpy(&s_count, v + 28, 4);
        memcpy(&offset, v + 32, 4);
        memcpy(&size, v + 36, 4);
        memcpy(&count, v + 40, 4);
        if (s_size < 264 || (uint64_t)s_off + (uint64_t)s_size * s_count > mbi.RegionSize)
            s_count = 0;                       /* readings without sensor names */
        if (size >= 292 && (uint64_t)offset + (uint64_t)size * count <= mbi.RegionSize) {
            ok = 1;
            for (i = 0; i < count; i++) {
                const unsigned char *r = v + offset + (size_t)size * i;
                char user[129], orig[129], s_user[129] = "", s_orig[129] = "";
                Reading rd;
                memcpy(user, r + (size >= 444 ? 316 : 140), 128);
                memcpy(orig, r + 12, 128);
                user[128] = orig[128] = 0;
                memcpy(&at, r + 4, 4);
                if (at < s_count) {
                    const unsigned char *sn = v + s_off + (size_t)s_size * at;
                    memcpy(s_user, sn + (s_size >= 392 ? 264 : 136), 128);
                    memcpy(s_orig, sn + 8, 128);
                    s_user[128] = s_orig[128] = 0;
                }
                rd.sensor[0] = s_user, rd.sensor[1] = s_orig;
                rd.label[0] = user, rd.label[1] = orig;
                memcpy(&rd.value, r + 284, 8);
                visit(ctx, &rd);
            }
        }
    }
    if (v)
        UnmapViewOfFile(v);
    CloseHandle(h);
    return ok;
}

static int hwinfo_running(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    int found = 0;
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe))
        do
            found = !_wcsnicmp(pe.szExeFile, L"HWiNFO", 6);
        while (!found && Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

/* Every "Report value in Gadget" reading: HKCU\SOFTWARE\HWiNFO64\VSB holds
   SensorN, LabelN and ValueRawN for each. There is no timestamp, so the
   stamp is a hash of every value; see monitor_take for how a stamp that
   stops changing is read. */
static int reg_each(Visit visit, void *ctx, uint64_t *stamp)
{
    enum { MAXV = 512 };
    static char names[MAXV][192], datas[MAXV][768];   /* room for 3 bytes a character */
    HKEY key;
    DWORD i, n = 0;
    uint64_t hash = 1469598103934665603ULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\HWiNFO64\\VSB", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return 0;
    for (i = 0; n < MAXV; i++) {
        wchar_t wname[64], wdata[256];
        DWORD nlen = 64, dlen = sizeof wdata, type;
        LONG rc = RegEnumValueW(key, i, wname, &nlen, NULL, &type,
                                (BYTE *)wdata, &dlen);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS || type != REG_SZ)
            continue;
        wname[63] = wdata[255] = 0;
        if (!WideCharToMultiByte(CP_UTF8, 0, wname, -1, names[n], 192, NULL, NULL)
            || !WideCharToMultiByte(CP_UTF8, 0, wdata, -1, datas[n], 768, NULL, NULL))
            continue;
        n++;
    }
    RegCloseKey(key);
    for (i = 0; i < n; i++) {
        DWORD k;
        const char *c;
        if (!strncmp(names[i], "ValueRaw", 8))
            for (c = datas[i]; *c; c++)
                hash = (hash ^ (unsigned char)*c) * 1099511628211ULL;
        if (strncmp(names[i], "Label", 5))
            continue;
        {
            const char *raw = NULL, *sensor = "";
            for (k = 0; k < n; k++) {
                if (!strncmp(names[k], "ValueRaw", 8) && !strcmp(names[k] + 8, names[i] + 5))
                    raw = datas[k];
                if (!strncmp(names[k], "Sensor", 6) && !strcmp(names[k] + 6, names[i] + 5))
                    sensor = datas[k];
            }
            if (raw) {
                char num[64], *p, *end;
                Reading rd;
                snprintf(num, sizeof num, "%.63s", raw);
                for (p = num; *p; p++)
                    if (*p == ',')
                        *p = '.';
                rd.value = strtod(num, &end);
                rd.sensor[0] = rd.sensor[1] = sensor;
                rd.label[0] = rd.label[1] = datas[i];
                if (end != num)
                    visit(ctx, &rd);
            }
        }
    }
    {   /* Values that hold still do not mean HWiNFO has stopped: on an
           idle PC every reported value can. While they hold still, ask
           Windows whether HWiNFO is running, and if it is, keep the stamp
           moving. */
        static uint64_t last;
        uint64_t raw = hash;
        if (hash == last && hwinfo_running())
            hash ^= GetTickCount64();
        last = raw;
    }
    *stamp = hash;
    return n > 0;
}
#else
static int shm_each(Visit visit, void *ctx, uint64_t *stamp)
{
    (void)visit, (void)ctx, (void)stamp;
    return 0;
}

static int reg_each(Visit visit, void *ctx, uint64_t *stamp)
{
    (void)visit, (void)ctx, (void)stamp;
    return 0;
}
#endif

/* Where readings come from, the one to try first on top: shared memory
   (every edition while it is switched on), then the registry (free, no
   time limit). */
static const struct {
    const char *name;
    int (*each)(Visit visit, void *ctx, uint64_t *stamp);
} SOURCES[] = { { "read_shared_memory", shm_each }, { "read_registry", reg_each } };
#define NSOURCES (sizeof SOURCES / sizeof SOURCES[0])

/* The three readings wanted, from the first source that has any of them.
   Returns 0 when HWiNFO cannot be read; the stamp changes whenever HWiNFO
   has polled again. */
static int read_hwinfo(Want *w, uint64_t *stamp)
{
    size_t r;
    for (r = 0; r < NSOURCES; r++) {
        w->have[0] = w->have[1] = w->have[2] = 0;
        if (SOURCES[r].each(want_visit, w, stamp) && (w->have[0] || w->have[1] || w->have[2]))
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ readings */
typedef struct {
    int on, high;       /* in this state; reading at the level, since when */
    double since;
} Level;

typedef struct {
    int has;            /* 0 = no reading: shown as -- */
    double value, bar;  /* bar 0..1, eased towards the value */
    Level alarm, blink; /* the double border; the blinking contents */
    double fed;
} Channel;

static void channel_feed(Channel *c, double raw, double now, double smoothing)
{
    if (!c->has || smoothing <= 0)
        c->value = raw;
    else
        c->value += (raw - c->value) * (1 - exp(-(now - c->fed) / smoothing));
    c->has = 1;
    c->fed = now;
}

static void channel_lose(Channel *c)
{
    c->has = 0;
    memset(&c->alarm, 0, sizeof c->alarm);
    memset(&c->blink, 0, sizeof c->blink);
}

/* On at `on` or above, once held for hold_s; off again below `off`. */
static void level_check(Level *l, double v, double on, double off, double hold, double now)
{
    if (l->on) {
        if (v < off)
            l->on = l->high = 0;
    } else if (v >= on) {
        if (!l->high) {
            l->high = 1;
            l->since = now;
        }
        l->on = now - l->since >= hold;
    } else
        l->high = 0;
}

/* The double border at `on`, clearing below `off`; the contents blink at
   `blink`, clearing the same distance below it. Both wait hold_s, and
   both go by the number as shown (decimals places of it), so a smoothed
   99.7 % shown as 100 counts as 100. */
static void channel_check(Channel *c, double on, double off, double blink, double hold,
                          double now, int decimals)
{
    double shown, step = decimals == 2 ? 0.01 : decimals == 1 ? 0.1 : 1;
    if (!c->has)
        return;
    /* Levels set the wrong way round still behave: the alarm always
       clears at least one step of the shown number below where it comes
       on, so a reading sitting on the limit cannot flicker, and a blink
       level below the alarm level counts as the alarm level. */
    off = off < on - step ? off : on - step;
    blink = blink > on ? blink : on;
    shown = round(c->value / step) * step;
    level_check(&c->alarm, shown, on, off, hold, now);
    level_check(&c->blink, shown, blink, blink - (on - off), hold, now);
}

/* Move the bar a share of the way each frame, like a meter needle. */
static void channel_slide(Channel *c, double target, double dt)
{
    c->bar += (target - c->bar) * (dt * 4 < 1 ? dt * 4 : 1);
}

/* What is on screen for one box: whole numbers only, so the picture is
   redrawn only when something visible changes. */
typedef struct {
    int has, value, dec, lit, alarm, blank;  /* value x 10^dec; blank: the blink's off half */
} Shown;

/* ------------------------------------------------------------- drawing */
#define W 960               /* the window: what the Lamptron reports */
#define H 400
#define SX 8                /* the bezel round the screen, px */
#define SW (W - 2 * SX)     /* screen width; its height is in Panel */

#define LINE_RATE 15734.264  /* NTSC lines a second: the ripple's frequency is counted as
                               if each row of the screen were one of them */
#define RIPPLE_RUN 3.0       /* the ripple runs this many thicknesses beyond the screen, 1.5
                                each side, where its wobble is down to 0.2 %: so it comes
                                in and goes out past the edges at full strength */
#define RIPPLE_TRAVEL(s, rows) ((rows) + RIPPLE_RUN * (s)->ripple_thickness)

/* The screen's rows, under the hidden ones. */
static int screen_rows(const Settings *s)
{
    int top, bottom;
    hidden_rows(s, &top, &bottom);
    return H - top - bottom - 2 * SX;
}

#define OLD_HUM_THICKNESS 96        /* what 1.3.0 had built in */
#define OLD_RIPPLE_THICKNESS 67
#define OLD_RIPPLE_HZ 874           /* 18 rows from crest to crest */

static void settings_from_old(Settings *s, const OldSettings *o)
{
    int ripple = o->got[OLD_RIPPLE_SPEED] || o->got[OLD_RIPPLE_INTENSITY], rows;
    Settings tidy;
    if (!ripple && !o->got[OLD_HUM_SPEED])
        return;
    if (o->got[OLD_HUM_SPEED] && !o->have[NEW_HUM_THICKNESS])
        s->hum_thickness = OLD_HUM_THICKNESS;
    if (ripple && !o->have[NEW_RIPPLE_THICKNESS])
        s->ripple_thickness = OLD_RIPPLE_THICKNESS;
    if (ripple && !o->have[NEW_RIPPLE_FREQUENCY])
        s->ripple_frequency = OLD_RIPPLE_HZ;
    tidy = *s;              /* the rows and thickness in use: s is not yet held to range */
    settings_tidy(&tidy);
    rows = screen_rows(&tidy);
    if (o->got[OLD_HUM_SPEED] && !o->have[NEW_HUM_PERIOD])
        s->hum_period = rows / o->old[OLD_HUM_SPEED];
    if (o->got[OLD_RIPPLE_SPEED] && !o->have[NEW_RIPPLE_PERIOD])
        s->ripple_period = RIPPLE_TRAVEL(&tidy, rows) / o->old[OLD_RIPPLE_SPEED];
    if (o->got[OLD_RIPPLE_INTENSITY] && !o->have[NEW_RIPPLE_AMPLITUDE])
        s->ripple_amplitude = o->old[OLD_RIPPLE_INTENSITY] * 2 * 100 / SW;
}

#define MARGIN 18           /* screen edge to box */
#define GUTTER 22           /* between boxes */
#define BLOCKS 16
#define UNLIT 41            /* unlit block at 16 % (41 of 255) */
#define RIPPLE_PX 2.0f       /* the static's widest sideways jitter of a row, at static 1 */
#define RIPPLE_SIGMA 0.6006f /* thickness (the width at half strength) to the rows from
                               the middle to where the wobble is 1/e: 1 / (2 sqrt(ln 2)) */
#define SPECKLE 0.7f         /* brightest static speck, at static 1 */
#define CRT_GAMMA 2.5f       /* light from a CRT goes as drive voltage to this */
#define BRIGHT_SHIFT 0.25f   /* the black level's shift in drive, at brightness 1 */
#define BRIGHT_DOWN 0.7f     /* and at -1: enough to take the characters, which the glow
                                drives past saturation, to half their light */
#define BRIGHT_STEPS 4096    /* the brightness curve's table, over light 0..BRIGHT_TOP */
#define BRIGHT_TOP 2.0f
static const unsigned char BEZEL[3] = { 0x0A, 0x0A, 0x09 };

/* Light is added and scaled in linear light and turned into the screen's
   sRGB only at the very end, as it combines in the real world. Strengths
   are linear; the mockup's sRGB look was matched by eye. */
static const float HALATION[3][2] = { { 1.0f, 0.6f }, { 4.5f, 0.3f }, { 12.0f, 0.14f } };
#define SPOT_GROWTH 0.8f     /* beam spot defocus at the corners */
#define SCANLINE_GAP 0.35f   /* gap rows keep 35 % of the light */
#define BEAM_BLOOM 0.3f      /* bright lines widen the beam into the gap */
#define SPOT_BLOOM 0.8f      /* contrast above 1: the spot spreads round bright parts */
#define HUM_GAIN 0.18f       /* beam current lift in the hum bar, at 1 */
#define HUM_RASTER 0.004f    /* and the raster's own faint glow, at 1 */
#define HAZE 0.5f            /* the centre haze's share of its colour's lift over the
                                background, leaving room for the glare */
#define GLARE 0.03f          /* veiling glare: the light the whole screen gains, as a share
                                of the picture's average light; a CRT whose dark areas
                                beside bright ones read about 33:1, a well-used one, where a
                                good one reads 100:1 */

static float LINEAR[256];          /* uint8 sRGB -> linear */
static unsigned char ENCODE[4096]; /* linear -> uint8 sRGB */

static float to_linear(float s)
{
    return s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
}

static void tables(void)
{
    int i;
    for (i = 0; i < 256; i++)
        LINEAR[i] = to_linear(i / 255.0f);
    for (i = 0; i < 4096; i++) {
        float v = i / 4095.0f;
        float s = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
        ENCODE[i] = (unsigned char)lroundf(255 * s);
    }
}

typedef struct {
    int x1, x2, lx, g1, g2;
    char label[TITLE_MAX * 4 + 3];      /* "[TITLE]", or empty */
} Box;

/* The font's glyph for a printable ASCII character; '?' for any other. */
static int glyph(unsigned char c)
{
    return c >= 32 && c < 127 ? c - 32 : '?' - 32;
}

/* The next glyph of UTF-8 text: the degree sign, printable ASCII, or '?'
   for anything else the font does not have. Advances *s. */
static int next_glyph(const char **s)
{
    unsigned char c = (unsigned char)**s;
    (*s)++;
    if (c == 0xC2 && (unsigned char)**s == 0xB0) {
        (*s)++;
        return 95;
    }
    while (c >= 0x80 && ((unsigned char)**s & 0xC0) == 0x80)
        (*s)++;                              /* the rest of a character it lacks */
    return glyph(c);
}

static int glyph_count(const char *s)
{
    int n = 0;
    while (*s)
        next_glyph(&s), n++;
    return n;
}

/* Three boxes of equal width fill the screen width. A title sits in
   brackets in a gap in the box's top edge: in from the left on the left
   box, centred in the middle, in from the right on the right. */
static void box_layout(Box b[3], const Settings *s)
{
    int side = (SW - 2 * MARGIN - 2 * GUTTER) / 3, i;
    for (i = 0; i < 3; i++) {
        const char *c;
        int k = 1, width;
        b[i].x1 = MARGIN + i * (side + GUTTER);
        b[i].x2 = b[i].x1 + side;
        b[i].label[0] = 0;
        for (c = s->box[i].title; *c && k < (int)sizeof b[i].label - 2; c++)
            b[i].label[k++] = (char)toupper((unsigned char)*c);   /* tidied to 14 */
        if (k == 1) {                        /* no title: an unbroken edge */
            b[i].lx = b[i].g1 = b[i].g2 = b[i].x1;
            continue;
        }
        b[i].label[0] = '[', b[i].label[k] = ']', b[i].label[k + 1] = 0;
        width = glyph_count(b[i].label) * 16;
        /* left box from the left, right box from the right, the middle
           centred; a full-length title is centred in any box, as it would
           otherwise run into the corner */
        b[i].lx = i == 1 || glyph_count(s->box[i].title) >= TITLE_MAX
                  ? (b[i].x1 + b[i].x2 - width) / 2
                  : i == 0 ? b[i].x1 + 26 : b[i].x2 - 22 - width;
        b[i].g1 = b[i].lx - 8;
        b[i].g2 = b[i].lx + width + 8;
    }
}

/* One frame of a degauss: the field of the coil round the tube, swinging
   and dying away, shakes the picture (dx, dy px at the middle, more at
   the edges), turns it (radians), bulges it (px at the corners), and
   pulls the beam off its spot, which shows as patches of light drifting
   across the screen (blot, their strength, and bu, bv, their drift). */
#define DEGAUSS_S 2.0
typedef struct {
    int on;
    float dx, dy, turn, bulge, blot, bu, bv;
} Degauss;

typedef struct {
    Settings s;
    int sy, sh;                     /* visible screen: top row and height */
    float *mapx, *mapy;             /* the curve's lookup */
    int32_t *tap;                   /* ... as a top-left pixel, -1 at an edge */
    float *wts;                     /* ... and its four bilinear weights */
    float *base, *ink;              /* linear RGB: background and ink colour */
    float *defocus, *gap;           /* spot growth, light left in a scanline gap */
    float *picture, *lit, *warped, *after;
    int have_after;
    float *bright;                  /* brightness: beam current in, light out */
    unsigned char *img;             /* the drawing, before light */
    unsigned char *out;             /* W x H BGRA, top row first */
    Box boxes[3];
    Degauss dg;                     /* this frame's, set before panel_frame */
} Panel;

static void panel_free(Panel *p)
{
    free(p->mapx), free(p->mapy), free(p->tap), free(p->wts), free(p->base), free(p->ink);
    free(p->defocus), free(p->gap), free(p->picture);
    free(p->lit), free(p->warped), free(p->after), free(p->img), free(p->bright);
    p->mapx = p->mapy = p->base = p->ink = p->defocus = p->gap = p->wts = NULL;
    p->tap = NULL;
    p->picture = p->lit = p->warped = p->after = p->bright = NULL;
    p->img = NULL;
}

/* A CRT's brightness control sets the grid bias: it moves the drive
   voltage that means black. Light goes as drive to the 2.5 power, so this
   works in drive: light to drive, shift, back to light. Turned up, black
   is no longer black and the whole raster glows in its scanlines; turned
   down, the faint parts (glow, unlit blocks, afterglow) go first and
   everything dims, the characters last, as on a real set turned right
   down. The table gives the light out for the beam current in, over
   0..BRIGHT_TOP; NULL at 0, where nothing changes. */
static float *brightness_table(double brightness)
{
    float shift = (brightness < 0 ? BRIGHT_DOWN : BRIGHT_SHIFT) * (float)brightness, *table;
    int x;
    if (brightness == 0)
        return NULL;
    table = must_alloc((BRIGHT_STEPS + 2) * sizeof(float));
    for (x = 0; x <= BRIGHT_STEPS + 1; x++) {
        float drive = powf(x * BRIGHT_TOP / BRIGHT_STEPS, 1 / CRT_GAMMA) + shift;
        table[x] = drive > 0 ? powf(drive, CRT_GAMMA) : 0;
    }
    return table;
}

/* Build everything that depends only on the settings: the curve's lookup,
   background, vignette, reflection, corners, spot growth and scanlines. */
static void panel_configure(Panel *p, const Settings *s)
{
    unsigned char ph[3][3];
    float fg[3], bg[3], haze[3], bezel[3];
    float k = (float)s->curvature, cx, cy, half = SW / 2.0f, edge_r2, radius;
    int x, y, c, top, bottom, n;
    panel_free(p);
    p->s = *s;
    hidden_rows(s, &top, &bottom);
    p->sy = top + SX;
    p->sh = screen_rows(s);
    n = SW * p->sh;
    box_layout(p->boxes, s);
    phosphor_colours(s->phosphor, ph);
    for (c = 0; c < 3; c++) {
        fg[c] = to_linear(ph[0][c] / 255.0f);
        bg[c] = to_linear(ph[1][c] / 255.0f);
        haze[c] = to_linear(ph[2][c] / 255.0f);
        bezel[c] = to_linear(BEZEL[c] / 255.0f);
    }
    if (!p->out)
        p->out = must_alloc((size_t)W * H * 4);
    for (x = 0; x < W * H; x++) {
        p->out[x * 4] = BEZEL[2], p->out[x * 4 + 1] = BEZEL[1];
        p->out[x * 4 + 2] = BEZEL[0], p->out[x * 4 + 3] = 255;
    }
    p->mapx = must_alloc(n * sizeof(float));
    p->mapy = must_alloc(n * sizeof(float));
    p->tap = must_alloc(n * sizeof(int32_t));
    p->wts = must_alloc(n * 4 * sizeof(float));
    p->base = must_alloc(n * 3 * sizeof(float));
    p->ink = must_alloc(n * 3 * sizeof(float));
    p->defocus = must_alloc(n * sizeof(float));
    p->gap = must_alloc(n * sizeof(float));
    p->picture = must_alloc(n * sizeof(float));
    p->lit = must_alloc(n * sizeof(float));
    p->warped = must_alloc(n * sizeof(float));
    p->after = must_alloc(n * sizeof(float));
    p->img = must_alloc(n);
    p->have_after = 0;
    p->bright = brightness_table(s->brightness);
    cx = SW / 2.0f, cy = p->sh / 2.0f;
    edge_r2 = (half - MARGIN) / half * (half - MARGIN) / half;
    radius = k > 0 ? 26 + 225 * k : k < 0 ? 20 : 14;
    for (y = 0; y < p->sh; y++)
        for (x = 0; x < SW; x++) {
            int i = y * SW + x;
            float px = x + 0.5f, py = y + 0.5f;
            float dx = (px - cx) / half, dy = (py - cy) / half;
            float r2 = dx * dx + dy * dy;
            /* the pixel shows the picture at (x, y) + k*half*(dx, dy)*(r2 - edge) */
            float pull = k * half * (r2 - edge_r2);
            float t = sqrtf(((px - cx) / (cx * 1.41421356f)) * ((px - cx) / (cx * 1.41421356f))
                            + ((py - cy) / (cy * 1.41421356f)) * ((py - cy) / (cy * 1.41421356f)));
            /* light falls off towards the edges of the glass */
            float vig = powf(1 - 0.62f * clampf((t - 0.52f) / 0.48f, 0, 1), 2.2f);
            float sx = (px - 0.24f * SW) / (0.6f * SW), sy = (py - 0.14f * p->sh) / (0.45f * p->sh);
            float shine = to_linear(0.06f * clampf(1 - sqrtf(sx * sx + sy * sy) / 0.7f, 0, 1));
            float qx = fabsf(px - cx) - (cx - radius), qy = fabsf(py - cy) - (cy - radius);
            int inside;
            float edge;
            qx = qx > 0 ? qx : 0, qy = qy > 0 ? qy : 0;
            inside = qx * qx + qy * qy <= radius * radius;
            p->mapx[i] = x + pull * dx;
            p->mapy[i] = y + pull * dy;
            {   /* worked out once here, so a frame is one lookup */
                int x0 = (int)floorf(p->mapx[i]), y0 = (int)floorf(p->mapy[i]);
                float fx = p->mapx[i] - x0, fy = p->mapy[i] - y0;
                p->tap[i] = x0 >= 0 && y0 >= 0 && x0 < SW - 1 && y0 < p->sh - 1
                            ? y0 * SW + x0 : -1;
                p->wts[i * 4] = (1 - fx) * (1 - fy), p->wts[i * 4 + 1] = fx * (1 - fy);
                p->wts[i * 4 + 2] = (1 - fx) * fy, p->wts[i * 4 + 3] = fx * fy;
            }
            for (c = 0; c < 3; c++) {
                /* the background colour with the haze, brightest in the
                   middle; frame_colour adds the glare */
                float back = bg[c] + HAZE * (haze[c] - bg[c]) * (1 - clampf(t / 0.78f, 0, 1));
                p->base[i * 3 + c] = inside ? back * vig + shine : bezel[c];
                p->ink[i * 3 + c] = inside ? vig * fg[c] : 0;
            }
            /* the beam spot grows with deflection: softer, gaps fill in */
            edge = clampf(r2 / (1 + (cy / half) * (cy / half)), 0, 1);
            p->defocus[i] = SPOT_GROWTH * edge;
            /* the light a scanline gap leaves here; which rows are gaps
               is worked out each frame, as the lines can drift */
            p->gap[i] = SCANLINE_GAP + (1 - SCANLINE_GAP) * 0.4f * edge;
        }
}

/* ---- drawing into the 8-bit picture */
static void fill(Panel *p, int x0, int y0, int x1, int y1, unsigned char v)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SW - 1) x1 = SW - 1;
    if (y1 > p->sh - 1) y1 = p->sh - 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            p->img[y * SW + x] = v;
}

/* Text with its cell's top-left at (x, y), every font pixel a scale x
   scale block: pixel-exact Fixedsys, no anti-aliasing. */
static void text(Panel *p, int x, int y, const char *s, int scale)
{
    for (; *s; x += 8 * scale) {
        const unsigned char *g = FONT[next_glyph(&s)];
        int r, b;
        for (r = 0; r < 16; r++)
            for (b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b))
                    fill(p, x + b * scale, y + r * scale, x + (b + 1) * scale - 1,
                         y + (r + 1) * scale - 1, 255);
    }
}

/* Rows (within the 16-row cell) that hold ink, over a whole string. */
static void ink_rows(const char *s, int *top, int *bottom)
{
    int r;
    *top = 16, *bottom = -1;
    while (*s) {
        const unsigned char *g = FONT[next_glyph(&s)];
        for (r = 0; r < 16; r++)
            if (g[r]) {
                if (r < *top) *top = r;
                if (r > *bottom) *bottom = r;
            }
    }
}

/* A 2 px box outline with a gap in its top edge for the label. */
static void outline(Panel *p, int x1, int y1, int x2, int y2, int g1, int g2)
{
    fill(p, g2, y1 - 1, x2, y1, 255);
    fill(p, x1, y1 - 1, g1, y1, 255);
    fill(p, x1 - 1, y1, x1, y2, 255);
    fill(p, x2 - 1, y1, x2, y2, 255);
    fill(p, x1, y2 - 1, x2, y2, 255);
}

/* The biggest number scale, a whole multiple of the 16 px grid, whose
   digits fit `room` px of height and at which every box's widest number
   (its bar_min or bar_max) and unit fit across it. One scale for all
   three, so the numbers stay on one line. */
static void number_scales(const Panel *p, int room, int *scale, int *unit)
{
    int inner = p->boxes[0].x2 - p->boxes[0].x1 - 16, s, b;   /* 8 px clear */
    for (s = 9; s >= 1; s--) {
        int u = s >= 6 ? 3 : s >= 2 ? 2 : 1, fits = 9 * s <= room;
        for (b = 0; b < 3 && fits; b++) {
            const BoxSet *x = &p->s.box[b];
            char lo[32], hi[32];
            int digits;
            snprintf(lo, sizeof lo, "%.*f", (int)x->decimals, x->bar_min);
            snprintf(hi, sizeof hi, "%.*f", (int)x->decimals, x->bar_max);
            digits = (int)(strlen(lo) > strlen(hi) ? strlen(lo) : strlen(hi));
            fits = digits * 8 * s + glyph_count(x->unit) * 8 * u <= inner;
        }
        if (fits) {
            *scale = s, *unit = u;
            return;
        }
    }
    *scale = 1, *unit = 1;
}

/* One box blur of radius r along rows (step 1) or columns (step w), with
   black beyond the edges, by a running sum: the same cost for any r. */
static void box_pass(const float *src, float *dst, int lines, int len, int along,
                     int across, int r)
{
    int l, i;
    float scale = 1.0f / (2 * r + 1);
    for (l = 0; l < lines; l++) {
        const float *s = src + (size_t)l * across;
        float *d = dst + (size_t)l * across;
        float sum = 0;
        for (i = 0; i <= r && i < len; i++)
            sum += s[(size_t)i * along];
        for (i = 0; i < len; i++) {
            d[(size_t)i * along] = sum * scale;
            if (i + r + 1 < len)
                sum += s[(size_t)(i + r + 1) * along];
            if (i - r >= 0)
                sum -= s[(size_t)(i - r) * along];
        }
    }
}

/* Gaussian blur of sigma px, as three box blurs each way (within a few
   per cent of a true Gaussian), so a wide halo costs no more than a
   narrow one. */
static void blur(const float *src, float *dst, float *tmp, int w, int h, float sigma)
{
    float ideal = sqrtf(12 * sigma * sigma / 3 + 1);
    int lo = (int)floorf(ideal), m, k;
    if (lo % 2 == 0)
        lo--;
    m = (int)lroundf((12 * sigma * sigma - 3 * lo * lo - 12 * lo - 9) / (-4.0f * lo - 4));
    memcpy(dst, src, (size_t)w * h * sizeof(float));
    for (k = 0; k < 3; k++) {
        int r = ((k < m ? lo : lo + 2) - 1) / 2;
        if (r < 1)
            continue;
        box_pass(dst, tmp, h, w, 1, w, r);     /* along rows */
        box_pass(tmp, dst, w, h, w, 1, r);     /* down columns */
    }
}

/* The header's lines as the panel shows them: those that are not blank,
   in capitals, cut to HEADER_MAX characters. Returns how many. */
static int header_lines(const Settings *s, HeaderLines lines)
{
    const char *from[2];
    int n = 0, i;
    from[0] = s->header1, from[1] = s->header2;
    if (!s->header)
        return 0;
    for (i = 0; i < 2; i++) {
        char *c;
        if (!from[i][strspn(from[i], " \t")])
            continue;                            /* blank */
        snprintf(lines[n], sizeof lines[n], "%s", from[i]);
        cut_chars(lines[n], sizeof lines[n], HEADER_MAX);
        for (c = lines[n]; *c; c++)
            *c = (char)toupper((unsigned char)*c);
        n++;
    }
    return n;
}

/* The panel drawn into p->img, 8 bits a pixel: header, boxes, numbers and
   bars, before any light. */
static void panel_draw(Panel *p, const Shown shown[3], int signal)
{
    int i, b, top, bottom = p->sh - 25, bar_top, scale, unit;
    HeaderLines lines;
    int nlines = header_lines(&p->s, lines);
    memset(p->img, 0, (size_t)SW * p->sh);
    for (i = 0; i < nlines; i++)
        text(p, SW / 2 - glyph_count(lines[i]) * 8, 6 + 32 * i, lines[i], 2);
    top = BOX_TOPS[nlines];
    bar_top = bottom - 22 - 32;
    number_scales(p, bar_top - 12 - (top + 34), &scale, &unit);
    for (b = 0; b < 3; b++) {
        const Box *x = &p->boxes[b];
        const char *unit_text = p->s.box[b].unit;
        char number[16];
        int ink_top, ink_bottom, width, base_y, left, inner;
        outline(p, x->x1, top, x->x2, bottom, x->g1, x->g2);
        if (shown[b].alarm)
            outline(p, x->x1 + 6, top + 6, x->x2 - 6, bottom - 6, x->g1, x->g2);
        text(p, x->lx, top - 15, x->label, 2);
        if (shown[b].blank)
            continue;
        /* the number centred on its ink in the space above the bar */
        if (shown[b].has)
            snprintf(number, sizeof number, "%.*f", shown[b].dec,
                     shown[b].value / (shown[b].dec == 2 ? 100.0 : shown[b].dec == 1 ? 10.0 : 1.0));
        else
            snprintf(number, sizeof number, "--");
        ink_rows(number, &ink_top, &ink_bottom);
        {   /* a reading wider than the bar's range allowed for: smaller, this box only */
            int s = scale, u = unit;
            for (;;) {
                width = (int)strlen(number) * 8 * s + glyph_count(unit_text) * 8 * u;
                if (width <= x->x2 - x->x1 - 16 || s == 1)
                    break;
                s--, u = s >= 6 ? 3 : s >= 2 ? 2 : 1;
            }
            base_y = (int)lround((top + 34 + bar_top - 12) / 2.0
                                 - ((ink_top - 13) + (ink_bottom + 1 - 13)) * s / 2.0);
            left = (int)lround((x->x1 + x->x2) / 2.0 - width / 2.0);
            text(p, left, base_y - 13 * s, number, s);
            text(p, left + (int)strlen(number) * 8 * s, base_y - 13 * u, unit_text, u);
        }
        inner = (x->x2 - 14) - (x->x1 + 14);
        for (i = 0; i < BLOCKS; i++) {
            int bx0 = x->x1 + 14 + (int)lround(i * (inner + 4) / (double)BLOCKS);
            int bx1 = x->x1 + 14 + (int)lround((i + 1) * (inner + 4) / (double)BLOCKS) - 4;
            fill(p, bx0, bar_top, bx1 - 1, bar_top + 31, i < shown[b].lit ? 255 : UNLIT);
        }
    }
    if (!signal) {
        static const char msg[] = "NO SIGNAL - START HWINFO64";
        fill(p, MARGIN, p->sh / 2 - 24, SW - MARGIN, p->sh / 2 + 24, 0);
        text(p, SW / 2 - (int)strlen(msg) * 8, p->sh / 2 - 16, msg, 2);
    }
}

/* What the phosphor emits for that drawing, into p->picture: the beam's
   spot, the glow round what is lit, and the contrast. */
static void panel_light(Panel *p)
{
    int n = SW * p->sh, i, h;
    double bloom = p->s.contrast > 1 ? p->s.contrast : 1;
    float *tmp = p->lit, *soft = p->warped;  /* free until the next frame */
    for (i = 0; i < n; i++)
        p->picture[i] = LINEAR[p->img[i]];
    /* The spot is bigger at the edges. Contrast turned up drives more beam
       current, and the spot grows with it (blooming): wider everywhere,
       and wider still round what is bright. */
    blur(p->picture, soft, tmp, SW, p->sh, 1.2f * (float)bloom);
    for (i = 0; i < n; i++) {
        float w = p->defocus[i] + SPOT_BLOOM * (float)(bloom - 1) * soft[i];
        p->picture[i] += (soft[i] - p->picture[i]) * (w < 1 ? w : 1);
    }
    if (p->s.glow > 0) {
        float *halo = must_alloc(n * sizeof(float));
        for (h = 0; h < 3; h++) {             /* halation: light adds */
            blur(p->picture, soft, tmp, SW, p->sh, HALATION[h][0] * (float)p->s.glow);
            for (i = 0; i < n; i++)
                halo[i] += HALATION[h][1] * soft[i];
        }
        for (i = 0; i < n; i++)
            p->picture[i] += halo[i];
        free(halo);
    }
    if (p->s.contrast != 1)                   /* the contrast knob: the signal's gain */
        for (i = 0; i < n; i++)
            p->picture[i] *= (float)p->s.contrast;
}

/* The glowing picture of the panel, before scanlines and the curve. */
static void panel_compose(Panel *p, const Shown shown[3], int signal)
{
    panel_draw(p, shown, signal);
    panel_light(p);
}

/* One frame of a ripple, per row: the sideways shift, and how much static
   rides on the beam there; and for a row whose shift differs from a
   neighbour's by more than RIPPLE_STEEP pixels, the shift at RIPPLE_SUB
   slices down it, so the row can be drawn as their average rather than
   one hard step. */
#define RIPPLE_SUB 4
#define RIPPLE_STEEP 1.0f
#define RIPPLE_STILL 0.01f   /* a row shifted less than this many pixels is left as it is */
typedef struct {
    float shift[H], speck[H], sub[H][RIPPLE_SUB];
    unsigned char steep[H];
} Ripple;

static uint32_t noise_state = 2463534242u;

static uint32_t noise(void)                  /* xorshift32: fast, good enough */
{
    noise_state ^= noise_state << 13;
    noise_state ^= noise_state >> 17;
    noise_state ^= noise_state << 5;
    return noise_state;
}

/* The noise started afresh from a time: the same moment gives the same
   static, so a ripple held by Pause animation holds its static too. */
static void noise_from(double seconds)
{
    noise_state = (uint32_t)(int64_t)(seconds * 1000) * 2654435761u | 1;
    noise(), noise();
}

/* A ripple `progress` (0..1) of the way down the screen (monitor_ripple
   turns it round to run up): a sideways wobble, a sine wave `wavelength`
   rows from crest to crest, inside a band whose strength follows a
   Gaussian across its thickness, at full strength all the way, with static
   in the same band (a random jitter of each row, and speckle on the beam,
   drawn in panel_frame). The wave is measured from the band's middle, so
   it travels with the band as one shape; ripple_frequency is its waves a
   second as the beam would meet them at LINE_RATE rows a second, so its
   wavelength is LINE_RATE / ripple_frequency rows, and its amplitude is
   a percentage of the screen's width. seconds: since the ripple began,
   for the drift: the crests move ripple_drift waves a second through the
   band, forwards (the way it runs) or, below 0, back. Counted in waves,
   the step from frame to frame is the same at any wavelength, and at the
   most, 6 a second, a fifth of a wave at 30 frames a second; much faster
   and the pattern jumps rather than moves. */
static void ripple_rows(Ripple *r, int rows, double progress, double seconds, const Settings *s)
{
    int y;
    float wave[H];
    float run = (float)(RIPPLE_RUN * s->ripple_thickness);
    float amplitude = (float)(s->ripple_amplitude / 100 * SW);
    float width = RIPPLE_SIGMA * (float)s->ripple_thickness;
    float front = -run / 2 + (rows + run) * (float)progress;
    float grain = (float)s->ripple_static;
    float k = (float)(6.2831853 * s->ripple_frequency / LINE_RATE);   /* 2 pi / wavelength */
    float drift = 6.2831853f * (float)fmod(seconds * s->ripple_drift, 1);   /* in waves */
    for (y = 0; y < rows; y++) {
        float d = (y - front) / width;
        float band = expf(-d * d);
        float jitter = ((noise() >> 8) / 16777216.0f * 2 - 1) * grain;
        int i;
        wave[y] = band * amplitude * sinf((y - front) * k - drift);
        r->shift[y] = wave[y] + band * RIPPLE_PX * 0.5f * jitter;
        r->speck[y] = band * grain;
        for (i = 0; i < RIPPLE_SUB; i++) {             /* the slices: the row's jitter */
            float yy = y - 0.5f + (i + 0.5f) / RIPPLE_SUB, dd = (yy - front) / width;
            r->sub[y][i] = expf(-dd * dd) * (amplitude * sinf((yy - front) * k - drift)
                                             + RIPPLE_PX * 0.5f * jitter);
        }
    }
    for (y = 0; y < rows; y++)      /* by the wave alone: slices cannot smooth the jitter */
        r->steep[y] = (y > 0 && fabsf(wave[y] - wave[y - 1]) > RIPPLE_STEEP)
                      || (y < rows - 1 && fabsf(wave[y + 1] - wave[y]) > RIPPLE_STEEP);
}

/* The stages of a frame, in the order the light passes through them. The
   first four work in place on p->lit, the frame's copy of the picture. */

/* Mains hum lifts the beam current in bands hum_bar_thickness rows tall,
   the top of one at row hum_y (below 0 for none) and the others a screen
   apart above it, as the hum comes round once a field; each is strongest
   in its middle and fades as a raised cosine to its edges, where the
   raster also glows faintly of its own. Bands as tall as the screen meet
   in one smooth sine down it, as real hum makes. */
static void frame_hum(Panel *p, int hum_y)
{
    float hum = (float)p->s.hum_intensity;
    int x, y, top, tall = (int)p->s.hum_thickness;
    if (hum_y < 0)
        return;
    for (top = hum_y; top + tall > 0; top -= p->sh)
        for (y = top < 0 ? 0 : top; y < top + tall && y < p->sh; y++) {
            float band = 0.5f - 0.5f * cosf(6.2831853f * (y - top + 0.5f) / tall);
            for (x = 0; x < SW; x++)
                p->lit[y * SW + x] = p->lit[y * SW + x] * (1 + HUM_GAIN * hum * band)
                                     + HUM_RASTER * hum * band;
        }
}

/* The brightness control: beam current in, light out, by its table. */
static void frame_brightness(Panel *p)
{
    int n = SW * p->sh, i;
    if (!p->bright)
        return;
    for (i = 0; i < n; i++) {
        float at = p->lit[i] * (BRIGHT_STEPS / BRIGHT_TOP);
        int k = (int)at;
        if (k >= BRIGHT_STEPS)
            k = BRIGHT_STEPS, at = (float)k;   /* past the top: clips anyway */
        p->lit[i] = p->bright[k] + (p->bright[k + 1] - p->bright[k]) * (at - k);
    }
}

/* The static that comes with a ripple: speckle on the beam, new each
   frame, mostly faint with a few bright. */
static void frame_static(Panel *p, const Ripple *ripple)
{
    int x, y;
    for (y = 0; y < p->sh; y++) {
        float amount = ripple->speck[y] * SPECKLE;
        if (amount < 0.002f)
            continue;
        for (x = 0; x < SW; x++) {
            float u = (noise() >> 8) / 16777216.0f, u2 = u * u;
            p->lit[y * SW + x] += amount * u2 * u2 * u2;
        }
    }
}

/* The phosphor saturates without changing colour; then the scanline gaps,
   part filled where a line is bright. A scanline is `pitch` rows and its
   lower half (rounded down) is the gap: 2 is one lit row and one gap, 1
   has no gaps. scan_off is how far the lines have drifted down; a row the
   drift has moved a gap part-way over is that much a gap. Contrast above
   1 widens the beam further into the gaps. */
static void frame_scanlines(Panel *p, double scan_off)
{
    int pitch = (int)p->s.scanline_px, lit_rows = pitch - pitch / 2, x, y, i;
    float fill = BEAM_BLOOM * (float)(p->s.contrast > 1 ? p->s.contrast : 1);
    for (y = 0; y < p->sh; y++) {
        /* the row runs from `at` to at + 1 within its scanline, whose gap
           runs from lit_rows to pitch */
        double at = fmod(y - scan_off, pitch), from, to;
        float cover;
        if (at < 0)
            at += pitch;
        from = at > lit_rows ? at : lit_rows;
        to = at + 1 < pitch ? at + 1 : pitch;
        cover = to > from ? (float)(to - from) : 0;
        for (x = 0, i = y * SW; x < SW; x++, i++) {
            float v = p->lit[i] < 1 ? p->lit[i] : 1, gap = 1 - cover * (1 - p->gap[i]);
            p->lit[i] = v * (gap + (1 - gap) * (fill * v < 1 ? fill * v : 1));
        }
    }
}

/* Afterglow, into p->after: each pixel shows the brighter of its light now
   and its fading light from before. afterglow_s is the time to fade to
   1/e^2 (14 %), where it stops reading as a glow over the background.
   dt: seconds since the last frame, or negative to start afresh. It comes
   before the curve and the ripple move the picture, so it follows the
   picture: a real phosphor fades in milliseconds, too fast to leave the
   unmoved picture showing under a wobble, and the longer afterglow here
   is only there for changes in the picture itself. */
static void frame_afterglow(Panel *p, double dt)
{
    int n = SW * p->sh, i;
    if (dt >= 0 && p->have_after && p->s.afterglow_s > 0) {
        float keep = (float)exp(-2 * dt / p->s.afterglow_s);
        for (i = 0; i < n; i++) {
            float g = p->after[i] * keep;
            p->after[i] = p->lit[i] > g ? p->lit[i] : g;
        }
    } else
        memcpy(p->after, p->lit, n * sizeof(float));
    p->have_after = 1;
}

/* One row as it stands with no ripple or degauss, into p->warped: by the
   lookup panel_configure worked out for the curve of the glass, or as it
   is where the glass is flat. */
static void still_row(Panel *p, int y)
{
    int i = y * SW, end = i + SW;
    if (p->s.curvature == 0) {
        memcpy(p->warped + i, p->after + i, SW * sizeof(float));
        return;
    }
    for (; i < end; i++) {
        const float *a = p->after + p->tap[i], *k = p->wts + i * 4;
        p->warped[i] = p->tap[i] < 0 ? 0
                       : a[0] * k[0] + a[1] * k[1] + a[SW] * k[2] + a[SW + 1] * k[3];
    }
}

/* The curve of the glass, into p->warped. */
static void frame_curve(Panel *p)
{
    int y;
    for (y = 0; y < p->sh; y++)
        still_row(p, y);
}

/* The light after the afterglow at (sx, sy), bilinear, black outside. */
static float warp_at(const Panel *p, float sx, float sy)
{
    int x0 = (int)floorf(sx), y0 = (int)floorf(sy), dy, dx;
    float fx = sx - x0, fy = sy - y0, v = 0;
    if (x0 >= 0 && y0 >= 0 && x0 < SW - 1 && y0 < p->sh - 1) {
        const float *a = p->after + y0 * SW + x0;
        return (a[0] * (1 - fx) + a[1] * fx) * (1 - fy) + (a[SW] * (1 - fx) + a[SW + 1] * fx) * fy;
    }
    for (dy = 0; dy < 2; dy++)
        for (dx = 0; dx < 2; dx++) {
            int xx = x0 + dx, yy = y0 + dy;
            if (xx >= 0 && yy >= 0 && xx < SW && yy < p->sh)
                v += p->after[yy * SW + xx] * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        }
    return v;
}

/* The curve with a ripple's sideways shift of every row and a degauss's
   shake, into p->warped (either may be absent): the same lookup, worked
   out afresh as they change each frame. A steep ripple row is the
   average of its slices, each shifted and placed down the row on its
   own, as a beam spot taller than a line would blend them, rather than
   one hard step from its neighbours. A row the ripple does not reach is
   drawn the usual, quicker way. */
static void frame_warp(Panel *p, const Ripple *ripple)
{
    const Degauss *d = &p->dg;
    float half = SW / 2.0f, cy = p->sh / 2.0f;
    int x, y, i;
    for (y = 0; y < p->sh; y++) {
        int smooth = ripple && ripple->steep[y];
        float shift = ripple ? ripple->shift[y] : 0, v = (y + 0.5f - cy) / half;
        if (!d->on && !smooth && fabsf(shift) < RIPPLE_STILL) {
            still_row(p, y);
            continue;
        }
        for (x = 0; x < SW; x++) {
            int j = y * SW + x;
            float ox = shift, oy = 0;
            if (d->on) {            /* stronger towards the edges, nearest the coil */
                float u = (x + 0.5f - half) / half, r2 = u * u + v * v, edge = 0.4f + r2;
                ox += d->dx * edge - d->turn * v * half + d->bulge * r2 * u;
                oy += d->dy * edge + d->turn * u * half + d->bulge * r2 * v;
            }
            if (!smooth) {
                p->warped[j] = warp_at(p, p->mapx[j] + ox, p->mapy[j] + oy);
                continue;
            }
            p->warped[j] = 0;
            for (i = 0; i < RIPPLE_SUB; i++)
                p->warped[j] += warp_at(p, p->mapx[j] + ripple->sub[y][i] - shift + ox,
                                        p->mapy[j] + oy - 0.5f + (i + 0.5f) / RIPPLE_SUB);
            p->warped[j] /= RIPPLE_SUB;
        }
    }
}

/* The light in the phosphor's colour over the background, as sRGB into
   p->out, with veiling glare: light and electrons scattered in the tube
   lift the whole screen by a share of the picture's average light. */
static void frame_colour(Panel *p, const float *light)
{
    static float bx[SW], by[H];          /* a degauss's patches: across, and down */
    int x, y;
    double sum = 0;
    float fog;
    for (x = 0; x < SW * p->sh; x++)
        sum += light[x];
    fog = GLARE * (float)(sum / (SW * p->sh));
    for (x = 0; x < SW; x++)
        bx[x] = p->dg.on ? sinf(5.0f * x / (SW / 2.0f) + p->dg.bu) : 0;
    for (y = 0; y < p->sh; y++)
        by[y] = p->dg.blot * sinf(9.0f * y / (SW / 2.0f) + p->dg.bv);
    for (y = 0; y < p->sh; y++) {
        unsigned char *o = p->out + ((size_t)(p->sy + y) * W + SX) * 4;
        for (x = 0; x < SW; x++, o += 4) {
            int j = y * SW + x, c;
            float v = (light[j] + fog) * (1 + by[y] * bx[x]);
            for (c = 0; c < 3; c++) {
                int code = (int)((p->base[j * 3 + c] + v * p->ink[j * 3 + c]) * 4095);
                code = code < 0 ? 0 : code > 4095 ? 4095 : code;
                o[2 - c] = ENCODE[code];       /* BGRA */
            }
        }
    }
}

/* One finished frame into p->out. hum_y: top row of the hum bar, or -1000
   for none. scan_off: rows the scanlines have drifted down, 0
   for where they start. ripple: this frame of a ripple, or NULL. dt:
   seconds since the last frame, or negative to start the afterglow
   afresh. */
static void panel_frame(Panel *p, int hum_y, double scan_off, const Ripple *ripple, double dt)
{
    const float *light = p->after;
    memcpy(p->lit, p->picture, (size_t)SW * p->sh * sizeof(float));
    frame_hum(p, hum_y);
    frame_brightness(p);
    if (ripple)
        frame_static(p, ripple);
    frame_scanlines(p, scan_off);
    frame_afterglow(p, dt);
    if (ripple || p->dg.on)
        frame_warp(p, ripple), light = p->warped;
    else if (p->s.curvature != 0)
        frame_curve(p), light = p->warped;
    frame_colour(p, light);
}

/* ------------------------------------------------------------ the state */
typedef struct {
    Settings s;
    Channel ch[3];
    int signal, paused;
    double anim_t, fresh_at, ripple_at, ripple_from, degauss_from;
    int have_fresh, rippling;
    uint64_t stamp;
    int have_stamp;
} Monitor;

/* The ripple keeps the animation's clock, anim_t (ripple_at, when the
   next is due, and ripple_from, when the one running began), so Pause
   animation holds a ripple where it is. */
static void monitor_schedule_ripple(Monitor *m)
{
    m->ripple_at = m->anim_t + uniform(m->s.ripple_min, m->s.ripple_max);
}

static void monitor_init(Monitor *m, const Settings *s)
{
    memset(m, 0, sizeof *m);
    m->s = *s;
    m->degauss_from = -1e9;
    monitor_schedule_ripple(m);
}

/* Whether a and b differ in a setting of the ripple itself, the gap
   between ripples aside. */
static int ripple_settings_differ(const Settings *a, const Settings *b)
{
    size_t i;
    for (i = 0; i < NFIELDS; i++)
        if (!strncmp(FIELDS[i].key, "ripple", 6) && strncmp(FIELDS[i].key, "ripple_gap", 10)
            && memcmp((const char *)a + FIELDS[i].off, (const char *)b + FIELDS[i].off,
                      field_size(&FIELDS[i])))
            return 1;
    return 0;
}

/* New settings. A ripple that is running keeps its place though its
   period changes, and the wait for the next one starts again only if the
   gap's settings changed. */
static void monitor_settings(Monitor *m, const Settings *s)
{
    double progress = (m->anim_t - m->ripple_from) / m->s.ripple_period;
    int gap = s->ripple_min != m->s.ripple_min || s->ripple_max != m->s.ripple_max;
    m->s = *s;
    if (m->rippling)
        m->ripple_from = m->anim_t - progress * s->ripple_period;
    if (gap)
        monitor_schedule_ripple(m);
}

/* A ripple in sight now, so the settings window can show the ripple's
   settings as they change: one that is running carries on, and otherwise
   one starts with its middle at the screen's edge. */
static void monitor_ripple_now(Monitor *m, int rows)
{
    double run = RIPPLE_RUN * m->s.ripple_thickness;
    if (m->rippling || !m->s.ripple)
        return;
    m->rippling = 1;
    m->ripple_from = m->anim_t - m->s.ripple_period * (run / 2) / (rows + run);
}

/* Take one read of HWiNFO: got = 0 when it could not be read. */
static void monitor_take(Monitor *m, int got, const Want *w, uint64_t stamp, double now)
{
    const Settings *s = &m->s;
    int stale, i;
    m->signal = got;
    if (got && (!m->have_stamp || stamp != m->stamp)) {
        m->stamp = stamp, m->have_stamp = 1;
        m->fresh_at = now, m->have_fresh = 1;
    }
    stale = !m->have_fresh || now - m->fresh_at > s->stale_after_s;
    for (i = 0; i < 3; i++) {
        const BoxSet *b = &s->box[i];
        if (stale || !got || !w->have[i])
            channel_lose(&m->ch[i]);
        else {
            channel_feed(&m->ch[i], w->value[i], now, s->smoothing_s);
            channel_check(&m->ch[i], b->alarm_on, b->alarm_off, b->blink_at, b->hold_s, now,
                          (int)b->decimals);
        }
    }
}

/* Advance the bars and the animation by dt seconds. */
static void monitor_step(Monitor *m, double dt)
{
    const Settings *s = &m->s;
    int i;
    for (i = 0; i < 3; i++) {
        double target = 0;
        if (m->ch[i].has)              /* the bar runs from bar_min to bar_max */
            target = (m->ch[i].value - s->box[i].bar_min)
                     / (s->box[i].bar_max - s->box[i].bar_min);
        target = target < 0 ? 0 : target > 1 ? 1 : target;
        channel_slide(&m->ch[i], target, dt);
    }
    if (!m->paused)
        m->anim_t += dt;
    if (!m->rippling && m->anim_t >= m->ripple_at) {
        m->rippling = 1;
        m->ripple_from = m->anim_t;
    }
}

/* Blinking: half a second on, half a second off, on the animation's
   clock, so Pause animation holds it as it is. */
static void monitor_shown(const Monitor *m, Shown out[3])
{
    int i;
    for (i = 0; i < 3; i++) {
        int dec = (int)m->s.box[i].decimals;
        double v = m->ch[i].value * (dec == 2 ? 100 : dec == 1 ? 10 : 1);
        v = v > 999999999 ? 999999999 : v < -99999999 ? -99999999 : v;   /* 9 digits */
        out[i].has = m->ch[i].has;
        out[i].value = m->ch[i].has ? (int)lround(v) : 0;
        out[i].dec = dec;
        out[i].lit = (int)lround(m->ch[i].bar * BLOCKS);
        out[i].alarm = m->ch[i].alarm.on;
        out[i].blank = m->ch[i].blink.on && fmod(m->anim_t, 1.0) >= 0.5;
    }
}

/* How far the scanlines have drifted down, in rows, less than one
   scanline: drifting up counts down from a whole scanline. */
static double monitor_scan_off(const Monitor *m, int pitch)
{
    double run;
    if (pitch < 2 || m->s.scan_speed <= 0)
        return 0;
    run = fmod(m->anim_t * m->s.scan_speed, pitch);
    return strcmp(m->s.scan_dir, "up") ? run : pitch - run;
}

/* Top row of one hum bar, 0 to rows: it moves a screen's height each
   period, and frame_hum draws the next a screen behind it. */
static int monitor_hum_y(const Monitor *m, int rows)
{
    int run;
    if (!m->s.hum_bar)
        return -1000;
    run = (int)(fmod(m->anim_t / m->s.hum_period, 1) * rows);
    return strcmp(m->s.hum_dir, "up") ? run : (rows - run) % rows;
}

/* A degauss `now`: the field swings several times a second in each
   direction, at different rates so the shake never repeats, and dies
   away over DEGAUSS_S seconds. Returns d->on. */
static int monitor_degauss(const Monitor *m, double now, Degauss *d)
{
    double t = now - m->degauss_from, e;
    memset(d, 0, sizeof *d);
    if (t < 0 || t >= DEGAUSS_S)
        return 0;
    e = exp(-2.5 * t / DEGAUSS_S) * (1 - t / DEGAUSS_S);
    d->on = 1;
    d->dx = (float)(14 * e * sin(6.2831853 * 9.3 * t));
    d->dy = (float)(9 * e * sin(6.2831853 * 7.1 * t + 1.3));
    d->turn = (float)(0.035 * e * sin(6.2831853 * 5.3 * t + 0.4));
    d->bulge = (float)(18 * e * sin(6.2831853 * 3.7 * t + 2.1));
    d->blot = (float)(0.45 * e);
    d->bu = (float)(1.7 * t), d->bv = (float)(-1.1 * t);
    return 1;
}

/* The same ripple run bottom to top: every row's values turned upside
   down, the slices down each row with them. */
static void ripple_flip(Ripple *r, int rows)
{
    int a, b, i;
    for (a = 0, b = rows - 1; a <= b; a++, b--) {
        float t, sa[RIPPLE_SUB], sb[RIPPLE_SUB];
        unsigned char st;
        t = r->shift[a], r->shift[a] = r->shift[b], r->shift[b] = t;
        t = r->speck[a], r->speck[a] = r->speck[b], r->speck[b] = t;
        st = r->steep[a], r->steep[a] = r->steep[b], r->steep[b] = st;
        memcpy(sa, r->sub[a], sizeof sa);
        memcpy(sb, r->sub[b], sizeof sb);
        for (i = 0; i < RIPPLE_SUB; i++)
            r->sub[a][i] = sb[RIPPLE_SUB - 1 - i], r->sub[b][i] = sa[RIPPLE_SUB - 1 - i];
    }
}

/* Fills r and returns 1 while a ripple is running. Paused, it is the same
   ripple every time, static and all. */
static int monitor_ripple(Monitor *m, Ripple *r, int rows)
{
    double seconds = m->anim_t - m->ripple_from, progress = seconds / m->s.ripple_period;
    if (!m->rippling)
        return 0;
    if (progress >= 1) {
        m->rippling = 0;
        monitor_schedule_ripple(m);
        return 0;
    }
    if (!m->s.ripple)
        return 0;
    noise_from(m->anim_t);
    ripple_rows(r, rows, progress, seconds, &m->s);
    if (!strcmp(m->s.ripple_dir, "up"))
        ripple_flip(r, rows);
    return 1;
}

/* ---------------------------------------------------------- the checks */
static int write_bmp(const char *path, const unsigned char *bgra)
{
    FILE *f = fopen(path, "wb");
    unsigned char head[54] = { 'B', 'M' };
    int row = W * 3, y, x;
    uint32_t size = 54 + row * H;
    if (!f)
        return 0;
    memcpy(head + 2, &size, 4);
    head[10] = 54, head[14] = 40;
    memcpy(head + 18, &(int32_t){ W }, 4);
    memcpy(head + 22, &(int32_t){ H }, 4);
    head[26] = 1, head[28] = 24;
    fwrite(head, 1, 54, f);
    for (y = H - 1; y >= 0; y--)              /* BMP rows run bottom up */
        for (x = 0; x < W; x++)
            fwrite(bgra + ((size_t)y * W + x) * 4, 1, 3, f);
    fclose(f);
    return 1;
}

static void preview(const char *path, const double values[3], const int has[3], int alarm)
{
    Settings s;
    Panel p;
    Shown shown[3];
    int i, signal = 0;
    settings_defaults(&s);
    memset(&p, 0, sizeof p);
    panel_configure(&p, &s);
    for (i = 0; i < 3; i++) {
        double frac = (values[i] - s.box[i].bar_min) / (s.box[i].bar_max - s.box[i].bar_min);
        frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
        shown[i].has = has[i];
        shown[i].value = (int)lround(values[i]);
        shown[i].lit = has[i] ? (int)lround(frac * BLOCKS) : 0;
        shown[i].alarm = has[i] && alarm;
        shown[i].blank = shown[i].dec = 0;
        signal |= has[i];
    }
    panel_compose(&p, shown, signal);
    panel_frame(&p, 120, 0, NULL, -1);
    write_bmp(path, p.out);
    printf("%s\n", path);
}

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            printf("FAILED line %d: %s\n", __LINE__, #c);                     \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static double frame_sum(const Panel *p)
{
    double sum = 0;
    int i;
    for (i = 0; i < W * H * 4; i++)
        sum += p->out[i];
    return sum;
}

/* The logic that is easy to break. Prints ok or the failing check. */
static int selftest(void)
{
    Channel c;
    Monitor m;
    Want w;
    Settings s;
    Panel p;
    Shown plain[3] = { { 1, 47, 0, 8, 0, 0 }, { 1, 61, 0, 8, 0, 0 }, { 1, 82, 0, 13, 0, 0 } },
          alarmed[3], dark[3];
    double sum_plain, sum_alarm, fading, settled = 0;
    Ripple off;
    Settings rs;
    int i, mid;
    /* alarm: needs the hold time, clears only below `off` */
    memset(&c, 0, sizeof c);
    channel_feed(&c, 96, 0, 0), channel_check(&c, 95, 90, 100, 10, 0, 0);
    CHECK(!c.alarm.on);
    channel_check(&c, 95, 90, 100, 10, 10, 0);
    CHECK(c.alarm.on && !c.blink.on);
    channel_feed(&c, 92, 11, 0), channel_check(&c, 95, 90, 100, 10, 11, 0);
    CHECK(c.alarm.on);
    channel_feed(&c, 89, 12, 0), channel_check(&c, 95, 90, 100, 10, 12, 0);
    CHECK(!c.alarm.on);
    /* blink: its own level, the same hold, clears 5 below it here */
    memset(&c, 0, sizeof c);
    channel_feed(&c, 100, 0, 0), channel_check(&c, 95, 90, 100, 0, 0, 0);
    CHECK(c.alarm.on && c.blink.on);
    channel_feed(&c, 96, 1, 0), channel_check(&c, 95, 90, 100, 0, 1, 0);
    CHECK(c.blink.on);
    channel_feed(&c, 94, 2, 0), channel_check(&c, 95, 90, 100, 0, 2, 0);
    CHECK(!c.blink.on && c.alarm.on);
    /* levels the wrong way round: a clear level above the alarm counts as
       the alarm level (no flicker), a blink level below it likewise */
    memset(&c, 0, sizeof c);
    channel_feed(&c, 97, 0, 0), channel_check(&c, 95, 99, 100, 0, 0, 0);
    CHECK(c.alarm.on && !c.blink.on);
    channel_feed(&c, 94, 1, 0), channel_check(&c, 95, 99, 100, 0, 1, 0);
    CHECK(c.alarm.on);                   /* at least 1 below: 94 holds it */
    channel_feed(&c, 93, 2, 0), channel_check(&c, 95, 95, 100, 0, 2, 0);
    CHECK(!c.alarm.on);                  /* equal levels: the same 1-unit gap */
    memset(&c, 0, sizeof c);
    channel_feed(&c, 96, 0, 0), channel_check(&c, 95, 90, 50, 0, 0, 0);
    CHECK(c.alarm.on && c.blink.on);
    /* decimals: the alarm goes by the number as shown, and its gap is one
       step of it */
    memset(&c, 0, sizeof c);
    channel_feed(&c, 1.404, 0, 0), channel_check(&c, 1.4, 1.35, 9, 0, 0, 2);
    CHECK(c.alarm.on);
    channel_feed(&c, 1.396, 1, 0), channel_check(&c, 1.4, 1.4, 9, 0, 1, 2);
    CHECK(c.alarm.on);                   /* 1.40 shown: still at the level */
    channel_feed(&c, 1.384, 2, 0), channel_check(&c, 1.4, 1.4, 9, 0, 2, 2);
    CHECK(!c.alarm.on);                  /* 1.38: below 1.39, one step down */
    /* readings by label, or by sensor and label */
    {
        Reading rd;
        rd.sensor[0] = "My GPU", rd.sensor[1] = "dGPU [#0]: NVIDIA";
        rd.label[0] = rd.label[1] = "GPU Core Load";
        CHECK(reading_matches("GPU Core Load", &rd));
        CHECK(reading_matches("dGPU [#0]: NVIDIA | GPU Core Load", &rd));
        CHECK(reading_matches("My GPU | GPU Core Load", &rd));
        CHECK(!reading_matches("GPU [#0]: AMD Radeon | GPU Core Load", &rd));
        CHECK(!reading_matches("GPU Core", &rd));
    }
    /* smoothing: one time constant gets ~63 % of the way */
    memset(&c, 0, sizeof c);
    channel_feed(&c, 0, 0, 3), channel_feed(&c, 100, 3, 3);
    CHECK(c.value > 62 && c.value < 64);
    /* staleness: a stamp that stops changing blanks the readings */
    settings_defaults(&s);
    monitor_init(&m, &s);
    w.value[0] = 50, w.value[1] = 60, w.value[2] = 70;
    w.have[0] = w.have[1] = w.have[2] = 1;
    monitor_take(&m, 1, &w, 1, 0);
    CHECK(m.ch[1].has && lround(m.ch[1].value) == 60);
    monitor_take(&m, 1, &w, 1, 11);
    CHECK(!m.ch[1].has && m.signal);
    monitor_take(&m, 0, &w, 1, 12);
    CHECK(!m.signal);
    /* bars slide; the hum bar wraps; a ripple starts when it is due */
    monitor_take(&m, 1, &w, 2, 13);
    for (i = 0; i < 60; i++)
        monitor_step(&m, 1 / 30.0);
    {
        Shown now_shown[3];
        monitor_shown(&m, now_shown);
        CHECK(now_shown[0].lit == 8 && now_shown[1].value == 60);
    }
    CHECK(monitor_hum_y(&m, 304) >= 0 && monitor_hum_y(&m, 304) < 304);
    {
        int down;
        strcpy(m.s.hum_dir, "down");
        down = monitor_hum_y(&m, 304);
        strcpy(m.s.hum_dir, "up");            /* up: the same run, mirrored */
        CHECK(monitor_hum_y(&m, 304) == (304 - down) % 304);
    }
    /* scanlines drift within one scanline, down or back up; none with no gaps */
    m.s.scan_speed = 2;
    m.anim_t = 1;
    strcpy(m.s.scan_dir, "down");
    CHECK(monitor_scan_off(&m, 5) == 2 && monitor_scan_off(&m, 1) == 0);
    strcpy(m.s.scan_dir, "up");
    CHECK(monitor_scan_off(&m, 5) == 3);
    m.s.scan_speed = 0;
    CHECK(monitor_scan_off(&m, 5) == 0);
    /* a ripple starts when it is due, on the animation's clock, and ends
       a period later */
    m.ripple_at = 0;
    m.s.ripple_period = 0.6;
    monitor_step(&m, 0);
    m.anim_t += 0.1;
    CHECK(monitor_ripple(&m, &off, 304));
    {   /* paused, it is held where it is, its static too; a frame on, it has moved */
        static Ripple held;
        m.s.ripple_static = 1;
        m.paused = 1;
        monitor_step(&m, 1 / 30.0);
        CHECK(monitor_ripple(&m, &held, 304) && monitor_ripple(&m, &off, 304));
        CHECK(!memcmp(held.shift, off.shift, 304 * sizeof(float)));
        m.paused = 0;
        monitor_step(&m, 1 / 30.0);
        CHECK(monitor_ripple(&m, &off, 304)
              && memcmp(held.shift, off.shift, 304 * sizeof(float)));
    }
    m.anim_t += 0.5;
    CHECK(!monitor_ripple(&m, &off, 304) && !m.rippling && m.ripple_at >= m.anim_t + 20);
    {   /* new settings: a running ripple keeps its place when its period
           changes, and only a change to the gap starts the wait again */
        Settings t = m.s;
        double due = m.ripple_at;
        t.glow = 0.5, monitor_settings(&m, &t);
        CHECK(m.ripple_at == due);
        t.ripple_max = 30, monitor_settings(&m, &t);
        CHECK(m.ripple_at != due && m.ripple_at <= m.anim_t + 30);
        m.ripple_at = 0, monitor_step(&m, 0);
        m.anim_t += 0.3;                             /* half-way through 0.6 s */
        t.ripple_period = 10, monitor_settings(&m, &t);
        CHECK(fabs((m.anim_t - m.ripple_from) / m.s.ripple_period - 0.5) < 1e-9);
        m.anim_t += 5.1;
        CHECK(!monitor_ripple(&m, &off, 304));
        /* a ripple asked for now is in sight at once: its middle at the edge */
        CHECK(!ripple_settings_differ(&m.s, &t));
        t.ripple_static = 1, t.ripple_thickness = 50;
        CHECK(ripple_settings_differ(&m.s, &t));
        strcpy(t.ripple_dir, "down");
        monitor_settings(&m, &t);
        monitor_ripple_now(&m, 304);
        CHECK(monitor_ripple(&m, &off, 304) && off.speck[0] > 0.99f && off.speck[150] < 0.01f);
        m.anim_t += 10;
        CHECK(!monitor_ripple(&m, &off, 304));
    }
    m.s.ripple_period = 2;
    m.ripple_at = 0;
    monitor_step(&m, 0);
    m.anim_t += 1;
    {   /* up runs the same ripple bottom to top */
        static Ripple down;
        m.s.ripple_static = 0;
        strcpy(m.s.ripple_dir, "down");
        CHECK(monitor_ripple(&m, &down, 304));
        strcpy(m.s.ripple_dir, "up");
        CHECK(monitor_ripple(&m, &off, 304));
        for (i = 0; i < 304 && off.shift[i] == down.shift[303 - i]
                    && off.steep[i] == down.steep[303 - i]
                    && off.sub[i][0] == down.sub[303 - i][RIPPLE_SUB - 1]
                    && off.sub[i][1] == down.sub[303 - i][RIPPLE_SUB - 2]; i++)
            ;
        CHECK(i == 304 && down.shift[100] != down.shift[200]);
    }
    m.anim_t += 0.9;
    CHECK(monitor_ripple(&m, &off, 304));
    m.anim_t += 0.2;
    CHECK(!monitor_ripple(&m, &off, 304));
    {   /* degauss: off until asked; then a shake that dies away and ends */
        Degauss d0, d1, d2;
        CHECK(!monitor_degauss(&m, 31, &d0));
        m.degauss_from = 30;
        CHECK(monitor_degauss(&m, 30.05, &d0) && monitor_degauss(&m, 31.9, &d1));
        CHECK(fabsf(d0.dx) + fabsf(d0.dy) + fabsf(d0.bulge) > 5 && d1.blot < d0.blot / 10);
        CHECK(!monitor_degauss(&m, 30 + DEGAUSS_S, &d2) && !d2.on);
        m.degauss_from = -1e9;
    }
    /* settings: junk snaps back, gaps swap, bad kinds keep defaults */
    settings_defaults(&s);
    settings_parse(&s, "\xEF\xBB\xBF{\"glow\": \"lots\", \"curvature\": 9, \"header\": \"yes\","
                       " \"ripple_gap_min_s\": 50, \"ripple_gap_max_s\": 30,"
                       " \"phosphor\": \"pink\", \"fps\": 60, \"scanline_px\": 2.7,"
                       " \"ripple_direction\": \"left\", \"hum_bar_direction\": \"down\","
                       " \"box2_reading\": \"CPU \\\"Die\\\"\", \"box1_bar_max\": 0,"
                       " \"box3_title\": \"A VERY LONG TITLE INDEED\"}");
    settings_tidy(&s);
    CHECK(s.glow == 1 && s.curvature == 0.15 && s.header == 0);
    CHECK(!strcmp(s.phosphor, "fallout") && s.fps == 30 && s.scanline_px == 2);
    CHECK(s.ripple_min == 30 && s.ripple_max == 50);
    CHECK(!strcmp(s.ripple_dir, "up") && !strcmp(s.hum_dir, "down"));
    CHECK(!strcmp(s.box[1].reading, "CPU \"Die\""));
    CHECK(s.box[0].bar_max == 1 && glyph_count(s.box[2].title) == TITLE_MAX);
    settings_defaults(&s);
    /* 1.3.0's speeds and strength become times and a percentage, and a
       file with both keeps the new */
    settings_defaults(&s);
    settings_parse(&s, "{\"hum_bar_speed\": 40, \"ripple_speed\": 850, \"ripple_intensity\": 1,"
                       " \"ripple_period_s\": 0.7}");
    CHECK(fabs(s.hum_period - 304 / 40.0) < 1e-9 && s.ripple_period == 0.7
          && fabs(s.ripple_amplitude - 200.0 / 944) < 1e-9);
    /* and what 1.3.0 had built in comes with them */
    CHECK(s.hum_thickness == 96 && s.ripple_thickness == 67 && s.ripple_frequency == 874);
    /* today's settings in the file are kept; an old key with no number is
       not read; rows hidden beyond reason are held to range first; and
       what was read before the reader stops is still converted */
    settings_defaults(&s);
    settings_parse(&s, "{\"ripple_speed\": 1000, \"ripple_thickness\": 100,"
                       " \"ripple_intensity\": null, \"visible_top\": 1e300, \"glow\": [1],"
                       " \"hum_bar_speed\": 40}");
    settings_tidy(&s);
    CHECK(s.ripple_thickness == 100 && s.ripple_frequency == 874 && s.ripple_amplitude == 3);
    CHECK(fabs(s.ripple_period - (224 + 300) / 1000.0) < 1e-9 && s.hum_period == 16.7
          && s.hum_thickness == 200);
    settings_defaults(&s);
    settings_parse(&s, "{\"phosphor\": \"#12ab34\", \"box2_unit\": \"" DEG "F\"}");
    settings_tidy(&s);
    CHECK(!strcmp(s.phosphor, "#12ab34") && !strcmp(s.box[1].unit, DEG "F"));
    settings_parse(&s, "{\"box1_unit\": \"MB/s/x" DEG DEG "C\"}");  /* 9 characters */
    settings_tidy(&s);
    CHECK(!strcmp(s.box[0].unit, "MB/s/x" DEG DEG));
    /* \u escapes come through as UTF-8; big numbers keep 10 figures */
    settings_defaults(&s);
    settings_parse(&s, "{\"box2_unit\": \"\\u00b0F\", \"box1_title\": \"A\\u00e9\\ud83d\\ude00\","
                       " \"box1_bar_max\": 123456.7}");
    settings_tidy(&s);
    CHECK(!strcmp(s.box[1].unit, DEG "F") && !strcmp(s.box[0].title, "A\xC3\xA9\xF0\x9F\x98\x80"));
    {
        char buf[64];
        snprintf(buf, sizeof buf, "%.10g", s.box[0].bar_max);
        CHECK(!strcmp(buf, "123456.7"));
    }
    /* hidden rows: a two-line header leaves the numbers at least 9 rows */
    settings_defaults(&s);
    settings_parse(&s, "{\"header\": true, \"header_line1\": \"A\", \"header_line2\": \"B\","
                       " \"visible_top\": 150, \"visible_bottom\": 10}");
    settings_tidy(&s);
    {
        int top, bottom;
        hidden_rows(&s, &top, &bottom);
        CHECK(top == 150 && bottom == 7 && s.visible_bottom == 10);
    }
    /* the header: 52 characters whatever bytes they take, capitals, and
       blank lines left out */
    {
        HeaderLines lines;
        settings_defaults(&s);
        s.header = 1;
        for (i = 0; i < 30; i++)
            strcat(s.header1, DEG);
        strcpy(s.header2, "   ");
        CHECK(header_lines(&s, lines) == 1 && glyph_count(lines[0]) == 30);
        memset(s.header1, 'x', 60), s.header1[60] = 0;
        CHECK(header_lines(&s, lines) == 1 && glyph_count(lines[0]) == HEADER_MAX
              && lines[0][0] == 'X');
    }
    /* curve: barrel shows nearer the centre in the middle, further out at
       the corners; pincushion the reverse; the side edges stay put */
    settings_defaults(&s);
    s.curvature = 0.08;
    memset(&p, 0, sizeof p);
    panel_configure(&p, &s);
    mid = p.sh / 2;
    CHECK(p.mapx[mid * SW + 100] > 100 && p.mapx[2 * SW + 2] < 2);
    CHECK(fabsf(p.mapx[mid * SW + MARGIN] - MARGIN) < 0.3f);
    /* a frame: the alarm adds the second frame line */
    panel_compose(&p, plain, 1);
    panel_frame(&p, 0, 0, NULL, -1);
    sum_plain = frame_sum(&p);
    for (i = 0; i < 3; i++)
        alarmed[i] = plain[i], alarmed[i].alarm = 1, dark[i].has = 0, dark[i].lit = 0,
        dark[i].alarm = dark[i].blank = 0, dark[i].value = dark[i].dec = 0;
    panel_compose(&p, alarmed, 1);
    panel_frame(&p, 0, 0, NULL, -1);
    sum_alarm = frame_sum(&p);
    CHECK(sum_alarm > sum_plain);
    /* brightness: up lifts black, so the whole raster glows; down dims */
    {
        double plain_sum;
        Settings b = s;
        panel_compose(&p, plain, 1);
        panel_frame(&p, 0, 0, NULL, -1);
        plain_sum = frame_sum(&p);
        b.brightness = 1, panel_configure(&p, &b);
        panel_compose(&p, plain, 1), panel_frame(&p, 0, 0, NULL, -1);
        CHECK(frame_sum(&p) > plain_sum * 1.05);
        b.brightness = -1, panel_configure(&p, &b);
        panel_compose(&p, plain, 1), panel_frame(&p, 0, 0, NULL, -1);
        CHECK(frame_sum(&p) < plain_sum * 0.92);   /* the characters dim too */
        /* contrast: up strengthens and spreads what is lit; down dims */
        b = s, b.contrast = 2, panel_configure(&p, &b);
        panel_compose(&p, plain, 1), panel_frame(&p, 0, 0, NULL, -1);
        CHECK(frame_sum(&p) > plain_sum * 1.05);
        b.contrast = 0.5, panel_configure(&p, &b);
        panel_compose(&p, plain, 1), panel_frame(&p, 0, 0, NULL, -1);
        CHECK(frame_sum(&p) < plain_sum * 0.98);   /* the background stays */
        /* veiling glare: the background between the boxes lifts as more
           of the screen is lit */
        {
            int gap_px = ((size_t)(p.sy + 80) * W + SX + 430) * 4 + 1, quiet;
            panel_configure(&p, &s);
            panel_compose(&p, dark, 1), panel_frame(&p, -1000, 0, NULL, -1);
            quiet = p.out[gap_px];
            panel_compose(&p, plain, 1), panel_frame(&p, -1000, 0, NULL, -1);
            CHECK(p.out[gap_px] > quiet);
        }
        panel_configure(&p, &s);
    }
    /* the blink's off half leaves the contents out, the frames in */
    for (i = 0; i < 3; i++)
        alarmed[i].blank = 1;
    panel_compose(&p, alarmed, 1);
    panel_frame(&p, 0, 0, NULL, -1);
    CHECK(frame_sum(&p) < sum_plain);
    /* scanlines drifted a whole scanline are where they started; part-way, not */
    {
        unsigned char *still = must_alloc((size_t)W * H * 4);
        int pitch = (int)p.s.scanline_px;
        panel_compose(&p, plain, 1);
        panel_frame(&p, -1000, 0, NULL, -1);
        memcpy(still, p.out, (size_t)W * H * 4);
        panel_frame(&p, -1000, pitch, NULL, -1);
        CHECK(!memcmp(still, p.out, (size_t)W * H * 4));
        panel_frame(&p, -1000, 0.5, NULL, -1);
        CHECK(memcmp(still, p.out, (size_t)W * H * 4));
        free(still);
    }
    /* static adds speckle where the ripple is */
    rs = s, rs.ripple_amplitude = 0.2, rs.ripple_static = 0;
    ripple_rows(&off, p.sh, 0.5, 0.25, &rs);
    panel_compose(&p, plain, 1);
    panel_frame(&p, 0, 0, &off, -1);
    sum_plain = frame_sum(&p);
    {   /* the wavelength: a sine crosses zero twice a wavelength, so 80
           rows through the ripple's middle cross 4 times at 40, 8 at 20 */
        int lambda, crossings, y;
        for (lambda = 20; lambda <= 40; lambda += 20) {
            rs.ripple_frequency = LINE_RATE / lambda;
            ripple_rows(&off, 304, 0.5, 0.25, &rs);
            for (y = 113, crossings = 0; y < 192; y++)
                crossings += (off.shift[y] < 0) != (off.shift[y - 1] < 0);
            CHECK(abs(crossings - 160 / lambda) <= 1);
        }
    }
    {   /* the thickness: the width where the wobble's band is at half strength */
        int t, y, half;
        rs.ripple_amplitude = 0, rs.ripple_static = 1;   /* speck follows the band alone */
        for (t = 30; t <= 120; t += 90) {
            rs.ripple_thickness = t;
            ripple_rows(&off, 304, 0.5, 0.25, &rs);
            for (y = 0, half = 0; y < 304; y++)
                half += off.speck[y] >= 0.5f;    /* the band is 1 at its middle */
            CHECK(abs(half - t) <= 2);
        }
        rs = s, rs.ripple_amplitude = 0.2;
    }
    {   /* the drift: at 0 the wave moves with the band as one shape (the
           middle 505 rows on: 101 rows by progress 0.2), and otherwise
           its crests move through the band */
        float before;
        rs.ripple_static = 0, rs.ripple_drift = 0, rs.ripple_thickness = 67;
        ripple_rows(&off, 304, 0.4, 0.25, &rs);
        before = off.shift[150];
        ripple_rows(&off, 304, 0.6, 0.90, &rs);
        CHECK(fabsf(off.shift[251] - before) < 1e-3f && fabsf(before) > 0.1f);
        rs.ripple_drift = 3;
        ripple_rows(&off, 304, 0.5, 0.25, &rs);
        before = off.shift[150];
        ripple_rows(&off, 304, 0.5, 0.30, &rs);
        CHECK(fabsf(off.shift[150] - before) > 0.1f);
        rs.ripple_drift = s.ripple_drift;
    }
    rs.ripple_static = 2;
    ripple_rows(&off, p.sh, 0.5, 0.25, &rs);
    panel_frame(&p, 0, 0, &off, -1);
    CHECK(frame_sum(&p) > sum_plain);
    {   /* a row the ripple does not reach is drawn as with no ripple, to
           the last bit; static alone makes no row steep */
        float *plain_row = must_alloc(SW * sizeof(float));
        rs = s, rs.ripple_static = 0, rs.ripple_thickness = 10, rs.ripple_frequency = 3000;
        ripple_rows(&off, p.sh, 0.05, 0.25, &rs);
        panel_compose(&p, plain, 1), panel_frame(&p, -1000, 0, NULL, -1);
        memcpy(plain_row, p.warped + 250 * SW, SW * sizeof(float));
        panel_frame(&p, -1000, 0, &off, -1);
        CHECK(off.shift[250] == 0 && fabsf(off.shift[2]) > 1
              && !memcmp(plain_row, p.warped + 250 * SW, SW * sizeof(float)));
        free(plain_row);
        rs = s, rs.ripple_amplitude = 0, rs.ripple_static = 1, rs.ripple_thickness = 100;
        ripple_rows(&off, p.sh, 0.5, 0.25, &rs);
        for (i = 0; i < p.sh && !off.steep[i]; i++)
            ;
        CHECK(i == p.sh);
    }
    {   /* smoothing: a gentle wave has no steep rows and draws as before;
           a steep one blends its rows, so the picture changes */
        unsigned char *hard = must_alloc((size_t)W * H * 4);
        rs = s, rs.ripple_static = 0, rs.ripple_amplitude = 0.05, rs.ripple_frequency = 500;
        ripple_rows(&off, p.sh, 0.5, 0.25, &rs);
        for (i = 0; i < p.sh && !off.steep[i]; i++)
            ;
        CHECK(i == p.sh);
        rs.ripple_amplitude = 5, rs.ripple_frequency = 3000, rs.ripple_thickness = 100;
        ripple_rows(&off, p.sh, 0.5, 0.25, &rs);
        CHECK(off.steep[p.sh / 2]);
        panel_compose(&p, plain, 1), panel_frame(&p, -1000, 0, &off, -1);
        memcpy(hard, p.out, (size_t)W * H * 4);
        memset(off.steep, 0, sizeof off.steep);          /* each row one hard step */
        panel_compose(&p, plain, 1), panel_frame(&p, -1000, 0, &off, -1);
        CHECK(memcmp(hard, p.out, (size_t)W * H * 4));
        free(hard);
    }
    {   /* the afterglow follows the picture as a ripple moves it: a steady
           picture shows nothing of where it was before the ripple */
        unsigned char *fresh = must_alloc((size_t)W * H * 4);
        rs.ripple_static = 0;
        ripple_rows(&off, p.sh, 0.5, 0.25, &rs);
        panel_compose(&p, plain, 1);
        panel_frame(&p, -1000, 0, &off, -1);
        memcpy(fresh, p.out, (size_t)W * H * 4);
        panel_frame(&p, -1000, 0, NULL, -1);
        for (i = 0; i < 10; i++)
            panel_frame(&p, -1000, 0, NULL, 1 / 30.0);
        panel_frame(&p, -1000, 0, &off, 1 / 30.0);
        {   /* the same, to a rounding of the last bit */
            size_t q;
            for (q = 0; q < (size_t)W * H * 4 && abs(fresh[q] - p.out[q]) <= 1; q++)
                ;
            CHECK(q == (size_t)W * H * 4);
        }
        free(fresh);
    }
    {   /* a degauss moves and patches the picture */
        unsigned char *still = must_alloc((size_t)W * H * 4);
        panel_compose(&p, plain, 1), panel_frame(&p, -1000, 0, NULL, -1);
        memcpy(still, p.out, (size_t)W * H * 4);
        p.dg.on = 1, p.dg.dx = 6, p.dg.turn = 0.01f, p.dg.blot = 0.3f;
        panel_frame(&p, -1000, 0, NULL, -1);
        CHECK(memcmp(still, p.out, (size_t)W * H * 4));
        memset(&p.dg, 0, sizeof p.dg);
        free(still);
    }
    /* afterglow: a number that goes out fades over frames */
    panel_compose(&p, dark, 1);
    panel_frame(&p, -1000, 0, NULL, 1 / 30.0);
    fading = frame_sum(&p);
    for (i = 0; i < 30; i++) {
        panel_frame(&p, -1000, 0, NULL, 1 / 30.0);
        settled = frame_sum(&p);
    }
    CHECK(fading > settled);
    s.curvature = -0.08;
    panel_configure(&p, &s);
    CHECK(p.mapx[mid * SW + 100] < 100);
    panel_free(&p);
    free(p.out);
    printf("ok\n");
    return 0;
}

/* ============================================================ Windows */
#ifdef _WIN32
#define WM_TRAY (WM_APP + 1)
enum { ID_EDIT = 1, ID_PAUSE, ID_DEGAUSS, ID_QUIT };

static struct {
    Panel panel;
    Monitor mon;
    Settings s;
    HWND win, tray_owner;
    NOTIFYICONDATAW nid;
    HICON icon;
    UINT taskbar_created;
    uint64_t mtime;
    int placed, have_place, have_shown;
    POINT place;
    Shown shown[3];
    int signal_shown;
    double last, poll_at, house_at;
    double save_due;            /* when to write the settings file; 0 = saved */
    char icon_colour[16];       /* the phosphor the icons were made in */
    UINT timer_ms;              /* the frame timer's interval in use */
    int save_failed;
    BITMAPINFO bmi;
} app;

/* -------------------------------------------------------- translations */
/* The settings window, its messages and the tray menu are written in
   English, and T() says them in the language in use. Each language is a
   catalogue, lang/<tag>.json: a flat {"English": "translation"} map with
   "_meta": {"name": ..., "native": ...}. The catalogues are built into
   the program (app.rc); a lang folder beside the settings file can
   replace one or add a language. A blank or missing entry stays English.
   N_() marks English kept in a table for T() to say later, so that
   lang/sync_english.py, which writes lang/en.json from this file, finds
   it. The panel itself stays English: its font has only the keyboard's
   characters and the degree sign. */
#define N_(text) text
#define IDR_LANG 200                /* RCDATA: IDR_LANG + i holds LANGS[i] */
static const char *const LANGS[] = { "en", "de", "es", "fr", "it", "ru", "ja", "zh-Hans" };
#define NLANGS (sizeof LANGS / sizeof LANGS[0])

typedef struct {
    wchar_t *en, *tr;
} Pair;

static struct {
    Pair *pair;
    int n;
    char tag[16];                   /* the catalogue in use; "en" for none */
    char chosen[16];                /* the setting that picked it */
} lang;

static const wchar_t *T(const wchar_t *english)
{
    int i;
    for (i = 0; i < lang.n; i++)
        if (!wcscmp(lang.pair[i].en, english))
            return lang.pair[i].tr;
    return english;
}

static wchar_t *wide_copy(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = must_alloc((size_t)(n > 0 ? n : 1) * sizeof *w);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    return w;
}

static void pairs_free(Pair *pair, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(pair[i].en), free(pair[i].tr);
    free(pair);
}

/* A catalogue's pairs, and its language's own name ("native"). Blank
   translations are left out unless keep_blank. Stops quietly at
   anything it cannot read. Returns the number of pairs. */
static int catalogue_parse(const char *text, Pair **out, char *native, size_t len,
                           int keep_blank)
{
    static char key[2048], value[2048];
    Json j;
    int n = 0, room = 0;
    *out = NULL;
    if (native)
        *native = 0;
    if (!json_open(&j, text))
        return 0;
    while (json_key(&j, key, sizeof key)) {
        if (*j.p == '{') {                      /* "_meta" */
            j.p++;
            while (json_key(&j, key, sizeof key)) {
                if (!json_string(&j, value, sizeof value))
                    return n;
                if (native && !strcmp(key, "native")) {
                    size_t k = strlen(value);
                    if (k > len - 1)                 /* cut to fit, whole characters */
                        for (k = len - 1; k && ((unsigned char)value[k] & 0xC0) == 0x80; k--)
                            ;
                    memcpy(native, value, k);
                    native[k] = 0;
                }
            }
            if (*j.p++ != '}')
                return n;
            continue;
        }
        if (!json_string(&j, value, sizeof value))
            return n;
        if (*key && (*value || keep_blank)) {
            if (n == room) {
                Pair *more = realloc(*out, (size_t)(room = room ? room * 2 : 256) * sizeof **out);
                if (!more)
                    return n;
                *out = more;
            }
            (*out)[n].en = wide_copy(key);
            (*out)[n].tr = wide_copy(value);
            n++;
        }
    }
    return n;
}

/* A catalogue's text from the lang folder beside the settings file,
   lang\<tag>.json; NULL if there is none. Free it after. */
static char *catalogue_file(const char *tag)
{
    char name[64], *text = NULL;
    FILE *f;
    snprintf(name, sizeof name, "lang\\%s.json", tag);
    if ((f = open_beside(name, "rb"))) {
        text = read_all(f);
        fclose(f);
    }
    return text;
}

/* The text of the catalogue built into the program for a tag; NULL if
   there is none. Free it after. */
static char *catalogue_builtin(const char *tag)
{
    size_t i;
    for (i = 0; i < NLANGS; i++)
        if (!strcmp(LANGS[i], tag)) {
            HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_LANG + i), MAKEINTRESOURCEW(10));
            HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
            const char *data = g ? LockResource(g) : NULL;
            DWORD size = r ? SizeofResource(NULL, r) : 0;
            char *text;
            if (!data)
                return NULL;
            text = must_alloc(size + 1);
            memcpy(text, data, size);
            return text;
        }
    return NULL;
}

/* A language's catalogue as its file has it, else as built in: for its
   name, and for --check-translations. NULL if there is neither. */
static char *catalogue_text(const char *tag)
{
    char *text = catalogue_file(tag);
    return text ? text : catalogue_builtin(tag);
}

/* The %-codes in a string, in order ("%ls%d"), so a translation can be
   checked to take the same values. A % that starts no code, as in
   "63 %", is text. */
static void codes(const wchar_t *text, char *out, size_t len)
{
    size_t n = 0;
    for (; *text; text++) {
        const wchar_t *end = text + 1;
        if (*text != L'%')
            continue;
        while (*end == L'l' || *end == L'h')
            end++;
        if (!*end || !wcschr(L"sdiuxXcfgeEp%", *end))
            continue;
        for (; text <= end && n + 1 < len; text++)
            out[n++] = (char)*text;
        text = end;
    }
    out[n] = 0;
}

/* The language Windows shows itself in, as a catalogue's tag, or "en"
   when there is no catalogue for it. Of Chinese, only the simplified
   kind has one. */
static void windows_language(char *tag, size_t len)
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    char utf8[LOCALE_NAME_MAX_LENGTH], *text;
    snprintf(tag, len, "en");
    if (!LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name,
                          LOCALE_NAME_MAX_LENGTH, 0))
        return;
    WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, NULL, NULL);
    if (!strncmp(utf8, "zh-", 3)) {
        if (!strcmp(utf8, "zh-CN") || !strcmp(utf8, "zh-SG") || !strncmp(utf8, "zh-Hans", 7))
            snprintf(tag, len, "zh-Hans");
        return;
    }
    utf8[strcspn(utf8, "-")] = 0;            /* "ja-JP": "ja" */
    if (*utf8 && strlen(utf8) < len && strcmp(utf8, "en") && (text = catalogue_text(utf8))) {
        free(text);
        memcpy(tag, utf8, strlen(utf8) + 1);
    }
}

/* A catalogue's pairs added after those in use, so that what was added
   first is found first; the text is freed. A translation that takes
   other values than its English (%ls and the like) is left out: it would
   be read wrongly where the values are filled in. Returns 0 for no text. */
static int lang_add(char *text)
{
    Pair *more, *all;
    int n, i;
    if (!text)
        return 0;
    n = catalogue_parse(text, &more, NULL, 0, 0);
    free(text);
    all = realloc(lang.pair, (size_t)(lang.n + n + 1) * sizeof *all);
    if (all)
        lang.pair = all;
    for (i = 0; i < n; i++) {
        char a[64], b[64];
        codes(more[i].en, a, sizeof a);
        codes(more[i].tr, b, sizeof b);
        if (all && !strcmp(a, b))
            lang.pair[lang.n++] = more[i];
        else
            free(more[i].en), free(more[i].tr);
    }
    free(more);
    return 1;
}

/* The language the setting names ("auto" for Windows') put in use: its
   file in the lang folder first, then the one built in, so a file need
   hold only what it changes. One with neither stays English. */
static void lang_use(const char *setting)
{
    char tag[16];
    int found;
    pairs_free(lang.pair, lang.n);
    lang.pair = NULL, lang.n = 0;
    snprintf(lang.chosen, sizeof lang.chosen, "%s", setting);
    if (!strcmp(setting, "auto"))
        windows_language(tag, sizeof tag);
    else
        snprintf(tag, sizeof tag, "%s", setting);
    found = lang_add(catalogue_file(tag));
    found |= lang_add(catalogue_builtin(tag));
    snprintf(lang.tag, sizeof lang.tag, "%s", found ? tag : "en");
}

#define MAX_LANGS 32                /* in the Language list */
typedef struct {
    char tag[16];
    wchar_t native[48];
} Language;

/* One language for the list: its tag, and its own name as its file gives
   it, else as built in, else its tag. */
static void language_add(Language *out, int *n, const char *tag)
{
    char native[96] = "";
    int builtin;
    for (builtin = 0; builtin < 2 && !*native; builtin++) {
        char *text = builtin ? catalogue_builtin(tag) : catalogue_file(tag);
        Pair *pair;
        if (text) {
            int got = catalogue_parse(text, &pair, native, sizeof native, 0);
            pairs_free(pair, got);
            free(text);
        }
    }
    snprintf(out[*n].tag, sizeof out[*n].tag, "%s", tag);
    MultiByteToWideChar(CP_UTF8, 0, *native ? native : tag, -1, out[*n].native, 48);
    out[*n].native[47] = 0;
    (*n)++;
}

/* The languages there are: English and the built-in ones in their
   order, then any other in the lang folder. Returns how many. */
static int languages(Language *out, int most)
{
    wchar_t pattern[LONG_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, k, known;
    size_t i;
    for (i = 0; i < NLANGS && n < most; i++)
        language_add(out, &n, LANGS[i]);
    beside("lang\\*.json", pattern, LONG_PATH);
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE)
        return n;
    do {
        char tag[64];
        wchar_t *dot = wcsrchr(fd.cFileName, L'.');
        if (dot)
            *dot = 0;
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, tag, sizeof tag, NULL, NULL);
        for (k = 0, known = 0; k < n; k++)
            known |= !_stricmp(out[k].tag, tag);
        if (!known && tag_ok(tag) && n < most)
            language_add(out, &n, tag);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void say_utf8(const char *before, const wchar_t *text)
{
    char utf8[4096];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof utf8, NULL, NULL);
    printf("%s%s\n", before, utf8);
}

/* --check-translations: each catalogue against lang/en.json, which holds
   every English string the program says. Lists what is missing, no
   longer used, or takes other values than the English (%-codes); a
   blank entry is counted, as it falls back to English. Exit code 1 if
   anything is missing, unused or wrong. */
static int check_translations(void)
{
    Language all[MAX_LANGS];
    Pair *en, *tr;
    char *text = catalogue_text("en");
    int nen, ntr, n, i, k, l, bad = 0;
    SetConsoleOutputCP(CP_UTF8);
    if (!text) {
        printf("lang/en.json is missing\n");
        return 1;
    }
    nen = catalogue_parse(text, &en, NULL, 0, 1);
    free(text);
    printf("English: %d strings\n", nen);
    n = languages(all, MAX_LANGS);
    for (l = 1; l < n; l++) {
        int missing = 0, blank = 0, unused = 0, wrong = 0;
        if (!(text = catalogue_text(all[l].tag)))
            continue;
        ntr = catalogue_parse(text, &tr, NULL, 0, 1);
        free(text);
        say_utf8("", all[l].native);
        for (i = 0; i < nen; i++) {
            for (k = 0; k < ntr && wcscmp(tr[k].en, en[i].en); k++)
                ;
            if (k == ntr) {
                missing++;
                say_utf8("  missing: ", en[i].en);
            } else if (!*tr[k].tr)
                blank++;
            else {
                char a[64], b[64];
                codes(en[i].en, a, sizeof a);
                codes(tr[k].tr, b, sizeof b);
                if (strcmp(a, b)) {
                    wrong++;
                    say_utf8("  other %-codes: ", en[i].en);
                }
            }
        }
        for (k = 0; k < ntr; k++) {
            for (i = 0; i < nen && wcscmp(tr[k].en, en[i].en); i++)
                ;
            if (i == nen) {
                unused++;
                say_utf8("  no longer used: ", tr[k].en);
            }
        }
        printf("  %s: %d translated, %d blank, %d missing, %d no longer used, %d with other "
               "%%-codes\n", all[l].tag, ntr - unused - blank, blank, missing, unused, wrong);
        bad |= missing || unused || wrong;
        pairs_free(tr, ntr);
    }
    pairs_free(en, nen);
    printf(bad ? "FAILED\n" : "ok\n");
    return bad;
}

/* ------------------------------------------------ screens, tray, frames */
/* Real pixels, so a 960 x 400 window is 960 x 400 on the Lamptron. */
static void dpi_aware(void)
{
    typedef BOOL(WINAPI * SetCtx)(HANDLE);
    SetCtx set = (SetCtx)(void *)GetProcAddress(GetModuleHandleW(L"user32"),
                                                "SetProcessDpiAwarenessContext");
    if (!set || !set((HANDLE)-4))             /* PER_MONITOR_AWARE_V2 */
        SetProcessDPIAware();
}

typedef struct {
    RECT r[16];
    int n;
} Monitors;

static BOOL CALLBACK each_monitor(HMONITOR mon, HDC dc, LPRECT rect, LPARAM data)
{
    Monitors *m = (Monitors *)data;
    MONITORINFO mi;
    (void)dc, (void)rect;
    mi.cbSize = sizeof mi;
    if (m->n < 16 && GetMonitorInfoW(mon, &mi))
        m->r[m->n++] = mi.rcMonitor;
    return TRUE;
}

static void monitors(Monitors *m)
{
    m->n = 0;
    EnumDisplayMonitors(NULL, NULL, each_monitor, (LPARAM)m);
}

/* Top-left corner of a monitor of exactly W x H. */
static int find_screen(POINT *at)
{
    Monitors m;
    int i;
    monitors(&m);
    for (i = 0; i < m.n; i++)
        if (m.r[i].right - m.r[i].left == W && m.r[i].bottom - m.r[i].top == H) {
            at->x = m.r[i].left, at->y = m.r[i].top;
            return 1;
        }
    return 0;
}

/* The tray icon: ">_" in the phosphor colour, drawn from the panel font. */
static HICON make_icon(const Settings *s)
{
    unsigned char ph[3][3];
    BITMAPV5HEADER bi;
    ICONINFO ii;
    HDC dc = GetDC(NULL);
    void *bits;
    HBITMAP color, mask;
    HICON icon;
    int x, y, i;
    phosphor_colours(s->phosphor, ph);
    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi, bi.bV5Width = 32, bi.bV5Height = -32;
    bi.bV5Planes = 1, bi.bV5BitCount = 32, bi.bV5Compression = BI_RGB;
    color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            unsigned char *o = (unsigned char *)bits + (y * 32 + x) * 4;
            int on = 0;
            for (i = 0; i < 2 && !on; i++) {          /* ">_" at 2x, centred */
                int gx = x - i * 16, gy = y;
                if (gx >= 0 && gx < 16)
                    on = FONT[glyph(i ? '_' : '>')][gy / 2] & (0x80 >> (gx / 2));
            }
            o[0] = ph[on ? 0 : 1][2], o[1] = ph[on ? 0 : 1][1], o[2] = ph[on ? 0 : 1][0];
            o[3] = 255;
        }
    mask = CreateBitmap(32, 32, 1, 1, NULL);
    ii.fIcon = TRUE, ii.xHotspot = ii.yHotspot = 0;
    ii.hbmMask = mask, ii.hbmColor = color;
    icon = CreateIconIndirect(&ii);
    DeleteObject(color), DeleteObject(mask);
    return icon;
}

static void tray_add(void)
{
    memset(&app.nid, 0, sizeof app.nid);
    app.nid.cbSize = sizeof app.nid;
    app.nid.hWnd = app.tray_owner;
    app.nid.uID = 1;
    app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    app.nid.uCallbackMessage = WM_TRAY;
    app.nid.hIcon = app.icon;
    wcscpy(app.nid.szTip, L"" NAME);
    Shell_NotifyIconW(NIM_ADD, &app.nid);
}

static void tray_menu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT at;
    AppendMenuW(menu, MF_STRING, ID_EDIT, T(L"Settings..."));
    AppendMenuW(menu, MF_STRING | (app.mon.paused ? MF_CHECKED : 0), ID_PAUSE,
                T(L"Pause animation"));
    AppendMenuW(menu, MF_STRING, ID_DEGAUSS, T(L"Degauss"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_QUIT, T(L"Quit"));
    GetCursorPos(&at);
    /* The owner must be in front, or the menu does not close when the
       user clicks elsewhere; the hidden owner window takes that, so the
       panel never does. */
    SetForegroundWindow(app.tray_owner);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, at.x, at.y, 0, app.tray_owner, NULL);
    PostMessageW(app.tray_owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* Frames at the chosen rate, or one a second while paused, unless a
   degauss is shaking the picture. Called on every frame and whenever one
   of those may have changed; it sets the timer only when the rate does. */
static void timer_set(void)
{
    Degauss d;
    int fast = !app.mon.paused || monitor_degauss(&app.mon, now_s(), &d);
    UINT ms = fast ? (UINT)(1000 / app.s.fps) : 1000;
    if (app.win && ms != app.timer_ms)
        SetTimer(app.win, 1, app.timer_ms = ms, NULL);   /* in place of the one running */
}

/* The degauss coil's sound, made here rather than kept in a file: a thunk
   as the current hits, the tube ringing, and the mains buzz dying away,
   peaking at under half of full scale in the thump, the buzz well below,
   so it stays quiet on small speakers.
   Mostly harmonics above 100 Hz, which small speakers can play. */
static void degauss_sound(void)
{
    enum { RATE = 22050, N = RATE * 21 / 10 };
    static unsigned char wav[44 + 2 * N];
    static float wave[N];
    static int made;
    if (!made) {
        static const unsigned char head[44] = {
            'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
            16, 0, 0, 0, 1, 0, 1, 0, RATE & 255, RATE >> 8, 0, 0,
            (2 * RATE) & 255, (2 * RATE) >> 8, (2 * RATE) >> 16, 0, 2, 0, 16, 0,
            'd', 'a', 't', 'a', 0, 0, 0, 0 };
        float peak = 0;
        int i, h;
        uint32_t size = 2 * N;
        for (i = 0; i < N; i++) {
            double t = (double)i / RATE, buzz = 0, v;
            double thunk = t < 0.3                    /* a thump, falling from 110 Hz */
                           ? 3 * tanh(3 * sin(6.2831853 * (110 - 162.5 * t) * t)) * exp(-t / 0.08)
                           : 0;
            double ring = 0.35 * sin(6.2831853 * 310 * t) * exp(-t / 0.25);
            double click = i < RATE / 200 ? ((noise() >> 8) / 8388608.0 - 1) * (1 - i * 200.0 / RATE)
                                          : 0;
            for (h = 1; h <= 9; h++)       /* 60 Hz and its harmonics: a buzz */
                buzz += sin(6.2831853 * 60 * h * t + h) / pow(h, 0.6);
            buzz *= 0.45 * (1 - exp(-t / 0.06)) * exp(-t / 0.55);   /* after the thump */
            v = (thunk + ring + 0.3 * click + buzz) * (1 - t / (N / (double)RATE));
            wave[i] = (float)v;
            peak = fabsf(wave[i]) > peak ? fabsf(wave[i]) : peak;
        }
        memcpy(wav, head, 44);
        wav[4] = (unsigned char)(size + 36), wav[5] = (unsigned char)((size + 36) >> 8);
        wav[6] = (unsigned char)((size + 36) >> 16);
        wav[40] = (unsigned char)size, wav[41] = (unsigned char)(size >> 8);
        wav[42] = (unsigned char)(size >> 16);
        for (i = 0; i < N; i++) {
            int v = (int)lrintf(wave[i] / peak * 0.45f * 32767);
            wav[44 + 2 * i] = (unsigned char)v, wav[45 + 2 * i] = (unsigned char)(v >> 8);
        }
        made = 1;
    }
    PlaySoundW((LPCWSTR)wav, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

static void blit(HDC dc)
{
    SetDIBitsToDevice(dc, 0, 0, W, H, 0, 0, 0, H, app.panel.out, &app.bmi, DIB_RGB_COLORS);
}

/* Onto the 960 x 400 screen whenever it is there, even if the panel
   started without it (Windows can bring a second screen up after the
   programs that start with it); out of sight while it is gone, rather
   than pushed onto another screen where it cannot be moved. */
static void place_window(void)
{
    POINT at;
    if (find_screen(&at)) {
        if (!app.placed) {
            ShowWindow(app.win, SW_HIDE);
            SetWindowLongPtrW(app.win, GWL_STYLE, WS_POPUP);
            SetWindowLongPtrW(app.win, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
            app.placed = 1, app.have_place = 0;
            log_note("the %dx%d monitor is here now; moved onto it", W, H);
        }
        if (!app.have_place || at.x != app.place.x || at.y != app.place.y
            || !IsWindowVisible(app.win)) {
            app.place = at, app.have_place = 1;
            SetWindowPos(app.win, NULL, at.x, at.y, W, H,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            ShowWindow(app.win, SW_SHOWNOACTIVATE);
        }
    } else if (app.placed && IsWindowVisible(app.win)) {
        ShowWindow(app.win, SW_HIDE);
        log_note("the %dx%d monitor has gone; the panel is hidden until it is back", W, H);
    }
}

/* The settings file, written a moment after the last change rather than
   on every keystroke or slider step. */
static void settings_window_reload(void);
static void settings_relabel(void);
static void settings_status(void);

/* If the file cannot be written (read-only, say, or a folder that cannot
   be written to), the change stays in effect, the save is tried again
   every 5 seconds, and the settings window's title says so. */
static void settings_save(void)
{
    int ok = write_beside(SLUG ".json", &app.s, 0, NFIELDS);
    if (!ok && !app.save_failed)
        log_note("could not save the settings file; trying again every 5 seconds");
    else if (ok && app.save_failed)
        log_note("the settings file is saved again");
    if (ok != !app.save_failed) {
        app.save_failed = !ok;
        settings_status();
    }
    app.mtime = mtime_beside(SLUG ".json");   /* no need to read it back */
    app.save_due = ok ? 0 : now_s() + 5;
}

static void apply_settings(void)
{
    if (strcmp(lang.chosen, app.s.language)) {   /* another language */
        lang_use(app.s.language);
        settings_relabel();
    }
    if (strcmp(app.icon_colour, app.s.phosphor)) {   /* the tray follows the colour */
        HICON old = app.icon;
        app.icon = make_icon(&app.s);
        snprintf(app.icon_colour, sizeof app.icon_colour, "%s", app.s.phosphor);
        app.nid.hIcon = app.icon;
        Shell_NotifyIconW(NIM_MODIFY, &app.nid);
        SetClassLongPtrW(app.win, GCLP_HICON, (LONG_PTR)app.icon);
        DestroyIcon(old);
    }
    monitor_settings(&app.mon, &app.s);
    panel_configure(&app.panel, &app.s);
    app.have_shown = 0;
    timer_set();
}

/* A degauss: the panel shakes for DEGAUSS_S seconds, even when paused,
   and the coil thunks and buzzes, quietly. */
static void degauss(void)
{
    app.mon.degauss_from = now_s();
    degauss_sound();
    timer_set();
}

static void tick(void)
{
    double now = now_s(), dt = now - app.last;
    Shown shown[3];
    Ripple ripple;
    int rippling;
    HDC dc;
    if (dt > 0.5)
        dt = 0.5;
    app.last = now;
    timer_set();
    if (now >= app.poll_at) {
        Want w;
        uint64_t stamp = 0;
        int got;
        app.poll_at = now + 0.5;
        w.labels[0] = app.s.box[0].reading;
        w.labels[1] = app.s.box[1].reading;
        w.labels[2] = app.s.box[2].reading;
        got = read_hwinfo(&w, &stamp);
        monitor_take(&app.mon, got, &w, stamp, now);
    }
    if (now >= app.house_at) {               /* settings saved; monitor moved */
        uint64_t mtime = mtime_beside(SLUG ".json");
        app.house_at = now + 2;
        if (mtime != app.mtime && !app.save_due) {   /* edited by hand */
            app.mtime = mtime;
            settings_load(&app.s, SLUG ".json");
            apply_settings();
            settings_window_reload();
        }
        place_window();
    }
    if (app.save_due && now >= app.save_due)
        settings_save();
    if (!IsWindowVisible(app.win))
        return;                             /* nothing to draw on */
    monitor_step(&app.mon, dt);
    monitor_shown(&app.mon, shown);
    if (!app.have_shown || memcmp(shown, app.shown, sizeof shown)
        || app.signal_shown != app.mon.signal) {
        memcpy(app.shown, shown, sizeof shown);
        app.signal_shown = app.mon.signal;
        app.have_shown = 1;
        panel_compose(&app.panel, shown, app.mon.signal);
    }
    rippling = monitor_ripple(&app.mon, &ripple, app.panel.sh);
    monitor_degauss(&app.mon, now, &app.panel.dg);
    panel_frame(&app.panel, monitor_hum_y(&app.mon, app.panel.sh),
                monitor_scan_off(&app.mon, (int)app.panel.s.scanline_px),
                rippling ? &ripple : NULL, dt);
    dc = GetDC(app.win);
    blit(dc);
    ReleaseDC(app.win, dc);
}

/* ----------------------------------------------------- settings window */
typedef struct {
    char label[129], sensor[129];
    double value;
} SeenOne;

typedef struct {
    SeenOne r[1024];
    int n;
} Seen;

static void seen_visit(void *ctx, const Reading *r)
{
    Seen *s = ctx;
    if (s->n < 1024) {
        snprintf(s->r[s->n].label, 129, "%s", r->label[0]);
        snprintf(s->r[s->n].sensor, 129, "%s", r->sensor[0]);
        s->r[s->n++].value = r->value;
    }
}

static int by_label(const void *a, const void *b)
{
    const SeenOne *x = a, *y = b;              /* label, then sensor */
    int c = strcmp(x->label, y->label);
    return c ? c : strcmp(x->sensor, y->sensor);
}

/* Every HWiNFO reading on offer now, sorted by label. */
static void collect_labels(Seen *seen)
{
    uint64_t stamp;
    size_t r;
    for (r = 0; r < NSOURCES; r++) {
        seen->n = 0;
        if (SOURCES[r].each(seen_visit, seen, &stamp) && seen->n)
            break;
    }
    qsort(seen->r, seen->n, sizeof seen->r[0], by_label);
}

/* The name to choose reading i by: its label, or its sensor and label if
   another sensor has the same label. 0 for a repeat of the one before. */
static int seen_name(const Seen *s, int i, char *out, size_t len)
{
    int before = i > 0 && !strcmp(s->r[i].label, s->r[i - 1].label);
    int after = i + 1 < s->n && !strcmp(s->r[i].label, s->r[i + 1].label);
    if (before && !strcmp(s->r[i].sensor, s->r[i - 1].sensor))
        return 0;
    if (before || after)
        snprintf(out, len, "%s" SENSOR_SEP "%s", s->r[i].sensor, s->r[i].label);
    else
        snprintf(out, len, "%s", s->r[i].label);
    return 1;
}

/* Start with Windows: a shortcut in the user's Startup folder, pointing at
   wherever this exe is now. The shortcut is the setting, so nothing about
   it goes in the settings file. */
static int startup_link(wchar_t *out)
{
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, out)))
        return 0;
    wcscat(out, L"\\" NAME L".lnk");
    return 1;
}

static int startup_on(void)
{
    wchar_t link[MAX_PATH + 32];
    return startup_link(link) && GetFileAttributesW(link) != INVALID_FILE_ATTRIBUTES;
}

static IShellLinkW *shell_link(void)
{
    IShellLinkW *sl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IShellLinkW, (void **)&sl)))
        return NULL;
    return sl;
}

static void startup_set(int on)
{
    wchar_t link[MAX_PATH + 32], exe[MAX_PATH], *slash;
    IShellLinkW *sl;
    IPersistFile *pf;
    if (!startup_link(link))
        return;
    if (!on) {
        DeleteFileW(link);
        return;
    }
    {
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            log_note("the program's path is too long for a Startup shortcut (260 characters)");
            return;
        }
    }
    if (!(sl = shell_link()))
        return;
    sl->lpVtbl->SetPath(sl, exe);
    if ((slash = wcsrchr(exe, L'\\')))
        *slash = 0;
    sl->lpVtbl->SetWorkingDirectory(sl, exe);
    sl->lpVtbl->SetDescription(sl, L"" NAME);
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        if (FAILED(pf->lpVtbl->Save(pf, link, TRUE)))
            log_note("could not write the Startup shortcut");
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
}

/* If the exe has moved since the shortcut was made, the shortcut follows
   it. A second copy run from elsewhere leaves a working shortcut alone. */
static void startup_follow(void)
{
    wchar_t link[MAX_PATH + 32], target[MAX_PATH] = L"";
    IShellLinkW *sl;
    IPersistFile *pf;
    int broken = 0;
    if (!startup_on() || !startup_link(link) || !(sl = shell_link()))
        return;
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        if (SUCCEEDED(pf->lpVtbl->Load(pf, link, STGM_READ))
            && SUCCEEDED(sl->lpVtbl->GetPath(sl, target, MAX_PATH, NULL, SLGP_RAWPATH)))
            broken = GetFileAttributesW(target) == INVALID_FILE_ATTRIBUTES;
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    if (broken)
        startup_set(1);
}

/* Control ids. A setting's control is IDC_FIELD + its field, and the
   number beside its slider IDC_READOUT + its field. */
enum { IDC_PHOSPHOR = 87, IDC_COLOUR, IDC_CLOSE, IDC_STARTUP, IDC_REVERT, IDC_PRESET,
       IDC_PRESET_SAVE, IDC_PRESET_DELETE, IDC_LINK, IDC_FOLDER, IDC_GAP, IDC_GAP_MAX, IDC_DEGAUSS,
       IDC_FIELD,                   /* 100: always after the last of those */
       IDC_READOUT = 1000 };
#define WM_APPLY (WM_APP + 2)
#define WM_RELANG (WM_APP + 3)      /* the language changed: build the window again */
enum { K_EDIT, K_CHECK, K_COMBO, K_LIST, K_SLIDER };

static struct {
    HWND win, tips;
    HWND ctl[NFIELDS], readout[NFIELDS];
    Language langs[MAX_LANGS];      /* the Language list, after Automatic */
    int nlangs;
    int kind[NFIELDS];
    Settings opened;            /* as they were when the window opened */
    int opened_startup;
    HFONT font;
    int dpi;                    /* the scale the window is laid out at */
    int screen_dpi;             /* its screen's own: the same unless that would not fit */
    int filling, pending;
    wchar_t decimal;            /* the user's decimal mark: '.' or ',' */
    COLORREF custom[16];
    HWND range, range_box[2];   /* the ripple gap's two-handle slider, and a number box
                                   for each handle: shortest left of it, longest right */
    int range_held, range_focus;    /* its handle being dragged (-1 for none), and the
                                       one the keys move: 0 shortest, 1 longest */
} gui;

static int S(int v)
{
    return MulDiv(v, gui.dpi, 96);
}

/* Tooltips: what each setting does, as a short phrase. Box settings are
   found by the part of the key after "boxN_". */
static const struct {
    const char *key;
    const wchar_t *tip;
} TIPS[] = {
    { "title", N_(L"Name in brackets on the box's top edge, up to 14 characters; empty for none") },
    { "reading",
      N_(L"The HWiNFO reading this box shows, by its label. The list holds what "
      L"HWiNFO offers now") },
    { "decimals",
      N_(L"Places after the decimal point: 0 for 61, 1 for 1.3, 2 for 1.25. The "
      L"alarm levels go by the number as shown") },
    { "unit",
      N_(L"Shown after the number: pick one from the list or type your own, up to"
      L" 8 characters. The panel's font has the keyboard's characters and °") },
    { "bar_min", N_(L"Reading at which the bar is empty") },
    { "bar_max", N_(L"Reading at which the bar is full") },
    { "alarm_on", N_(L"The border turns double at this reading or above") },
    { "alarm_off", N_(L"The double border clears when the reading falls below this") },
    { "blink_at",
      N_(L"The box's number and bar blink at this reading or above, and stop the "
      L"same distance below it as the alarm's gap") },
    { "alarm_hold_s",
      N_(L"Seconds the reading must stay at the alarm or blink level first; 0 for"
      L" at once") },
    { "curvature",
      N_(L"Screen curve, -0.15 to 0.15: positive bulges outwards (barrel), "
      L"negative pinches inwards (pincushion), 0 is flat") },
    { "glow", N_(L"Size of the glow round lit text and lines, 0 to 2; 0 switches it off") },
    { "brightness",
      N_(L"Like a CRT's brightness knob, which sets the black level: turned up, "
      L"the whole screen glows faintly in its scanlines; turned down, faint "
      L"glow and unlit blocks fade first. -1 to 1; 0 as designed") },
    { "contrast",
      N_(L"Like a CRT's contrast knob, which sets the picture's strength: turned up, the glow "
      L"and lit scanlines get stronger and the beam spot spreads round bright parts "
      L"(blooming); turned down, everything dims and black stays black. 0.5 to 2; 1 as "
      L"designed") },
    { "scanline_px", N_(L"Height of one scanline in pixels, 1 to 8; 1 turns the dark gaps off") },
    { "scanline_drift_direction", N_(L"Which way the scanlines drift") },
    { "scanline_drift_speed",
      N_(L"How fast the scanlines drift, in pixels per second, 0 to 30. 0 keeps them still, "
      L"as on a real CRT") },
    { "fps", N_(L"Frames per second; 15 halves the work") },
    { "hum_bar", N_(L"A soft bright band that rolls slowly up or down the screen, as mains hum "
                 L"does on an old set") },
    { "hum_bar_direction", N_(L"Which way the hum bar rolls") },
    { "hum_bar_period_s",
      N_(L"Seconds the hum bar takes to roll the screen's height, one following another, 1 to "
      L"600. 16.7 copies 60 Hz mains hum on a 59.94 Hz NTSC picture") },
    { "hum_bar_intensity", N_(L"How bright the hum bar is, 0 to 10") },
    { "hum_bar_thickness",
      N_(L"How tall the hum bar is, from edge to edge, in pixels, 8 to 400. It is brightest "
         L"in the middle") },
    { "ripple", N_(L"Now and then, a short sideways wobble that runs up or down the screen") },
    { "ripple_direction", N_(L"Which way the ripple runs") },
    { "ripple_amplitude_pct",
      N_(L"How far the ripple shifts the picture sideways each way, as a percentage of the "
         L"screen's width, 0 to 10") },
    { "ripple_frequency_hz",
      N_(L"The ripple's frequency, 10 to 3000 Hz, as if each row were a line of a 15.734 kHz "
         L"NTSC picture: the higher, the closer its wobbles") },
    { "ripple_thickness",
      N_(L"How tall the ripple's band is, in pixels, 10 to 300, measured where its wobble is "
         L"at half strength. The wobble fades away above and below") },
    { "ripple_drift_hz",
      N_(L"How fast the ripple's crests move through it, in hertz, -6 to 6: above 0 "
         L"forwards, below 0 back. At 0 the whole wave moves as one shape") },
    { "ripple_period_s",
      N_(L"Seconds the ripple takes to run across the screen, from coming into sight to "
      L"gone, 0.1 to 60") },
    { "ripple_static",
      N_(L"Static that comes with the ripple: speckle and a jitter of the lines, "
      L"0 to 1; 0 for none") },
    { "ripple_gap_min_s", N_(L"Shortest time between ripples, in seconds, 5 to 60") },
    { "ripple_gap_max_s",
      N_(L"Longest time between ripples, in seconds, 5 to 60; each gap is picked "
      L"at random between the two") },
    { "afterglow_s",
      N_(L"How long the phosphor keeps glowing after the picture changes, in "
      L"seconds, 0 to 4; 0 for none") },
    { "visible_top", N_(L"Rows of the 400 Windows reports that the display hides at the top") },
    { "visible_bottom",
      N_(L"Rows of the 400 Windows reports that the display hides at the bottom") },
    { "smoothing_s",
      N_(L"How quickly a box follows a change: after this many seconds it has "
      L"gone 63 % of the way to a new reading, after three times as long "
      L"nearly all the way. 0 shows each reading as it comes") },
    { "stale_after_s", N_(L"Seconds without a fresh reading before a box shows --") },
    { "header", N_(L"Show one or two lines of your own text above the boxes") },
    { "header_line1", N_(L"Up to 52 characters") },
    { "header_line2", N_(L"Up to 52 characters; leave empty for a one-line header") },
    { "language",
      N_(L"The language of this window and the tray menu. Automatic follows Windows") },
};

static const wchar_t *tip_for(const char *key)
{
    size_t i;
    if (!strncmp(key, "box", 3) && key[4] == '_')
        key += 5;
    for (i = 0; i < sizeof TIPS / sizeof TIPS[0]; i++)
        if (!strcmp(TIPS[i].key, key))
            return T(TIPS[i].tip);
    return L"";
}

static void tip(HWND ctl, const wchar_t *text)
{
    TOOLINFOW ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = gui.win;
    ti.uId = (UINT_PTR)ctl;
    ti.lpszText = (LPWSTR)text;
    SendMessageW(gui.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
                 int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y),
                             S(w), S(h), gui.win, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)gui.font, TRUE);
    return c;
}

static void label(const wchar_t *text, int x, int y, int w)
{
    make(L"STATIC", text, SS_LEFT, 0, x, y + 3, w, 20, -1);
}

static void group(const wchar_t *text, int x, int y, int w, int h)
{
    make(L"BUTTON", text, BS_GROUPBOX, 0, x, y, w, h, -1);
}

/* How far one step of a slider moves its setting. */
static double slider_step(const char *key)
{
    static const struct {
        const char *key;
        double step;
    } steps[] = {
        { "curvature", 0.01 }, { "glow", 0.05 }, { "brightness", 0.05 }, { "contrast", 0.05 },
        { "afterglow_s", 0.01 }, { "hum_bar_intensity", 0.05 },
        { "ripple_amplitude_pct", 0.01 }, { "ripple_static", 0.01 }, { "ripple_period_s", 0.01 },
        { "smoothing_s", 0.5 }, { "scanline_drift_speed", 0.1 }, { "ripple_drift_hz", 0.1 },
        { "hum_bar_period_s", 0.1 },
    };
    size_t i;
    for (i = 0; i < sizeof steps / sizeof steps[0]; i++)
        if (!strcmp(steps[i].key, key))
            return steps[i].step;
    return 1;
}

/* A slider's thumb at its setting, and the setting beside it as a number:
   as many places as the slider's step has, more if a typed number has
   them. */
static void slider_show(int i)
{
    const Field *f = &FIELDS[i];
    double v = *(const double *)((const char *)&app.s + f->off), step = slider_step(f->key);
    int places = step >= 1 ? 0 : step >= 0.1 ? 1 : 2;
    char text[32];
    SendMessageW(gui.ctl[i], TBM_SETPOS, TRUE, lround((v - f->lo) / step));
    while (places < 4 && fabs(v * pow(10, places) - floor(v * pow(10, places) + 0.5)) > 1e-6)
        places++;
    snprintf(text, sizeof text, "%.*f", places, v);
    SetWindowTextA(gui.readout[i], text);
    SendMessageW(gui.readout[i], EM_SETMODIFY, FALSE, 0);   /* shown, not typed */
}

/* The control for one setting, with its tooltip. A slider takes w for
   itself and its number together. */
static HWND control(const char *key, int kind, int x, int y, int w, const wchar_t *text)
{
    int i = settings_fields_find(key), id = IDC_FIELD + i;
    HWND c;
    gui.kind[i] = kind;
    if (kind == K_SLIDER) {
        const Field *f = &FIELDS[i];
        c = make(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_DOWNISLEFT, 0,
                 x - 4, y, w - 52, 24, id);
        SendMessageW(c, TBM_SETRANGEMIN, FALSE, 0);
        SendMessageW(c, TBM_SETRANGEMAX, TRUE, lround((f->hi - f->lo) / slider_step(key)));
        /* the number can be typed: taken on Enter or on leaving the box */
        gui.readout[i] = make(L"EDIT", L"", WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL,
                              WS_EX_CLIENTEDGE, x + w - 48, y, 48, 23, IDC_READOUT + i);
        SendMessageW(gui.readout[i], EM_LIMITTEXT, 12, 0);
        tip(gui.readout[i], tip_for(key));
    } else if (kind == K_EDIT)
        c = make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, x, y, w, 23, id);
    else if (kind == K_CHECK)
        c = make(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, 0, x, y, w, 23, id);
    else
        c = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL
                 | (kind == K_LIST ? CBS_DROPDOWNLIST : CBS_DROPDOWN | CBS_AUTOHSCROLL),
                 0, x, y, w, 320, id);
    gui.ctl[i] = c;
    tip(c, tip_for(key));
    if (kind == K_COMBO)                     /* the text part is its own child */
        tip(GetWindow(c, GW_CHILD), tip_for(key));
    return c;
}

/* The window's layout, in pixels at 100 % scaling: the boxes' grid on top;
   three groups of rows in the middle (Look, Animation, General); the
   header and the presets below them; the buttons at the foot. A row more
   in the tallest middle group is one more in MID_ROWS, and everything
   below moves down with it. */
enum {
    CLIENT_W = 1000,
    ROW = 26,                       /* from one row of a group to the next */
    COL1 = 12, COL2 = 340, COL3 = 668, COL_W = 320,    /* the middle groups */
    LABEL_W = 150,                  /* their label column: the longest translated
                                       labels (Spanish and German) fit it */
    FIELD_X = 12 + LABEL_W,         /* a group's left edge to its controls */
    LIST_W = 104,                   /* a short list: its longest entries ("Nach unten",
                                       "Vers le haut") fit at the smallest scale too */
    MID_Y = 334, MID_ROWS = 12, MID_H = 30 + MID_ROWS * ROW,
    LOW_Y = MID_Y + MID_H + 8, LOW_H = 106,             /* header and presets */
    BUTTONS_Y = LOW_Y + LOW_H + 12,
    CLIENT_H = BUTTONS_Y + 28 + 12
};

/* A label and a control on the row at *y of the group at gx; *y moves on
   to the next row. */
static HWND row(const wchar_t *text, const char *key, int kind, int gx, int *y, int w)
{
    HWND c;
    label(text, gx + 10, *y, LABEL_W);
    c = control(key, kind, gx + FIELD_X, *y, w, NULL);
    *y += ROW;
    return c;
}

/* A slider row: label, slider, number. */
static void slider(const wchar_t *text, const char *key, int gx, int *y)
{
    row(text, key, K_SLIDER, gx, y, 148);
}

/* A direction's list: Up first, as it is the default. */
static void directions(HWND c)
{
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(L"Up"));
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(L"Down"));
}

static void wide(HWND c, const char *utf8)
{
    wchar_t w[320];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 320);
    SetWindowTextW(c, w);
}

/* The phosphor list showing the preset in use, or Custom, which follows
   the presets. */
static void phosphor_fill(void)
{
    size_t i, sel = NPHOSPHORS;
    for (i = 0; i < NPHOSPHORS; i++)
        if (!strcmp(app.s.phosphor, PHOSPHORS[i].name))
            sel = i;
    SendMessageW(GetDlgItem(gui.win, IDC_PHOSPHOR), CB_SETCURSEL, sel, 0);
}

/* One setting's control showing what is in effect. */
static void gui_show(size_t i)
{
    const Field *f = &FIELDS[i];
    const char *at = (const char *)&app.s + f->off;
    char text[400];
    if (!gui.ctl[i])
        return;
    if (gui.kind[i] == K_SLIDER)
        slider_show((int)i);
    else if (!strcmp(f->key, "fps"))
        SendMessageW(gui.ctl[i], CB_SETCURSEL, app.s.fps < 23, 0);
    else if (!strcmp(f->key, "language")) {                /* Automatic, then each */
        int k, sel = 0;
        for (k = 0; k < gui.nlangs; k++)
            if (!strcmp(gui.langs[k].tag, app.s.language))
                sel = k + 1;
        SendMessageW(gui.ctl[i], CB_SETCURSEL, sel, 0);
    } else if (is_direction(f->key))                        /* Up, Down */
        SendMessageW(gui.ctl[i], CB_SETCURSEL, strcmp(at, "up") != 0, 0);
    else if (gui.kind[i] == K_LIST && f->type == T_INT)      /* decimals: 0, 1, 2 */
        SendMessageW(gui.ctl[i], CB_SETCURSEL, (WPARAM)*(const double *)at, 0);
    else if (f->type == T_BOOL)
        SendMessageW(gui.ctl[i], BM_SETCHECK, *(const double *)at ? BST_CHECKED : BST_UNCHECKED, 0);
    else {
        if (f->type == T_STR)
            snprintf(text, sizeof text, "%s", at);
        else
            snprintf(text, sizeof text, f->type == T_INT ? "%.0f" : "%.10g", *(const double *)at);
        wide(gui.ctl[i], text);
    }
}

/* Every control showing what is in effect. */
static void range_show(void);

static void gui_fill(void)
{
    size_t i;
    gui.filling = 1;
    for (i = 0; i < NFIELDS; i++)
        gui_show(i);
    phosphor_fill();
    range_show();
    SendMessageW(GetDlgItem(gui.win, IDC_STARTUP), BM_SETCHECK,
                 startup_on() ? BST_CHECKED : BST_UNCHECKED, 0);
    gui.filling = 0;
}

/* Revert is only lit when there is something to go back to. */
static void revert_state(void)
{
    if (gui.win)
        EnableWindow(GetDlgItem(gui.win, IDC_REVERT),
                     memcmp(&app.s, &gui.opened, sizeof app.s)
                     || startup_on() != gui.opened_startup);
}

/* A change made in the settings window: in effect at once, saved in a
   moment, and if it was to the ripple, a ripple to show it. */
static void save_and_apply(void)
{
    int ripple = ripple_settings_differ(&app.mon.s, &app.s);
    app.save_due = now_s() + 0.5;
    apply_settings();
    if (ripple)
        monitor_ripple_now(&app.mon, app.panel.sh);
    revert_state();
}

/* Everything back as it was when the window opened, Start with Windows
   included. */
static void revert(void)
{
    if (memcmp(&app.s, &gui.opened, sizeof app.s)) {
        app.s = gui.opened;
        save_and_apply();
    }
    if (startup_on() != gui.opened_startup)
        startup_set(gui.opened_startup);
    gui_fill();
    revert_state();
}

/* A typed number as C reads one: the user's decimal mark, as Windows'
   regional format has it (a comma in much of Europe), becomes a point.
   Where the mark is a point, a comma is left as it is, since there it
   separates thousands, and the number does not read. */
static void decimal_point(wchar_t *text)
{
    for (; *text; text++)
        if (*text == gui.decimal)
            *text = L'.';
}

/* Read every control into the settings. A number that does not parse yet
   (a "-" being typed, say) keeps its last value. */
static void gui_apply(void)
{
    Settings s = app.s;
    size_t i;
    for (i = 0; i < NFIELDS; i++) {
        const Field *f = &FIELDS[i];
        char *at = (char *)&s + f->off;
        wchar_t w[320];
        char text[400], *end;
        double v;
        if (!gui.ctl[i] || gui.kind[i] == K_SLIDER)   /* sliders: slider_moved */
            continue;
        if (!strcmp(f->key, "fps")) {
            s.fps = SendMessageW(gui.ctl[i], CB_GETCURSEL, 0, 0) == 1 ? 15 : 30;
            continue;
        }
        if (!strcmp(f->key, "language")) {
            LRESULT sel = SendMessageW(gui.ctl[i], CB_GETCURSEL, 0, 0);
            snprintf(at, f->len, "%s",
                     sel > 0 && sel <= gui.nlangs ? gui.langs[sel - 1].tag : "auto");
            continue;
        }
        if (is_direction(f->key)) {
            snprintf(at, f->len, "%s",
                     SendMessageW(gui.ctl[i], CB_GETCURSEL, 0, 0) == 1 ? "down" : "up");
            continue;
        }
        if (gui.kind[i] == K_LIST && f->type == T_INT) {     /* decimals */
            LRESULT sel = SendMessageW(gui.ctl[i], CB_GETCURSEL, 0, 0);
            if (sel >= 0)
                *(double *)at = (double)sel;
            continue;
        }
        if (f->type == T_BOOL) {
            *(double *)at = SendMessageW(gui.ctl[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
            continue;
        }
        GetWindowTextW(gui.ctl[i], w, 320);
        if (f->type != T_STR)
            decimal_point(w);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, text, sizeof text, NULL, NULL);
        if (f->type == T_STR) {
            snprintf(at, f->len, "%s", text);
            continue;
        }
        v = strtod(text, &end);
        if (end != text && !*end)
            *(double *)at = v;
    }
    settings_tidy(&s);
    if (memcmp(&s, &app.s, sizeof s)) {
        app.s = s;
        save_and_apply();
    }
}

/* A slider's setting set to v, out of range going to the nearer end, and
   every slider shown again. */
static void slider_set(int i, double v)
{
    const Field *f = &FIELDS[i];
    Settings s = app.s;
    size_t k;
    *(double *)((char *)&s + f->off) = v;
    settings_tidy(&s);
    if (memcmp(&s, &app.s, sizeof s)) {
        app.s = s;
        save_and_apply();
    }
    gui.filling = 1;
    for (k = 0; k < NFIELDS; k++)
        if (gui.kind[k] == K_SLIDER && gui.ctl[k])
            slider_show((int)k);
    gui.filling = 0;
}

/* A slider moved: its setting follows at once, so the panel changes as
   the thumb is dragged. */
static void slider_moved(HWND tb)
{
    int i = GetDlgCtrlID(tb) - IDC_FIELD;
    if (i >= 0 && i < (int)NFIELDS && gui.kind[i] == K_SLIDER)
        slider_set(i, FIELDS[i].lo
                      + SendMessageW(tb, TBM_GETPOS, 0, 0) * slider_step(FIELDS[i].key));
}

/* A number typed beside a slider, to as many places as were typed.
   Anything that is not a number puts back what was there. A box that only
   shows its slider's value is left alone: it shows at most four places,
   and reading that back would round a longer value. */
static void readout_typed(int i)
{
    wchar_t text[32], *end;
    double v;
    if (!SendMessageW(gui.readout[i], EM_GETMODIFY, 0, 0))
        return;
    GetWindowTextW(gui.readout[i], text, 32);
    decimal_point(text);
    v = wcstod(text, &end);
    while (*end == L' ')
        end++;
    if (end != text && !*end)
        slider_set(i, v);
    else {
        gui.filling = 1;
        slider_show(i);
        gui.filling = 0;
    }
}

/* The ripple gap's slider: one track with two handles, shortest and
   longest, which push each other rather than cross. It borrows its look
   and sizes from the ordinary sliders. Windows' own has one handle. */
static double *range_value(Settings *s, int which)
{
    return which ? &s->ripple_max : &s->ripple_min;
}

/* The handles' size and the track's place, as an ordinary slider has
   them, and how far the middle of a handle travels: as an ordinary
   slider's does, half a handle in from each end of the track. */
static void range_metrics(RECT *thumb, RECT *channel, RECT *travel)
{
    HWND like = gui.ctl[settings_fields_find("ripple_period_s")];
    int tw;
    SendMessageW(like, TBM_GETTHUMBRECT, 0, (LPARAM)thumb);
    SendMessageW(like, TBM_GETCHANNELRECT, 0, (LPARAM)channel);
    tw = thumb->right - thumb->left;
    *travel = *channel;
    travel->left += tw / 2, travel->right -= tw - tw / 2;
}

/* x of the middle of a value's handle, and the value nearest to an x. */
static int range_x(double v, const RECT *travel)
{
    const Field *f = &FIELDS[settings_fields_find("ripple_gap_min_s")];
    return travel->left
           + (int)lround((v - f->lo) / (f->hi - f->lo) * (travel->right - travel->left));
}

static double range_at(int x, const RECT *travel)
{
    const Field *f = &FIELDS[settings_fields_find("ripple_gap_min_s")];
    int span = travel->right - travel->left;
    double t = (double)(x - travel->left) / (span > 1 ? span : 1);
    return floor(f->lo + (t < 0 ? 0 : t > 1 ? 1 : t) * (f->hi - f->lo) + 0.5);
}

/* The gap as it is in effect: the two numbers in the boxes either side,
   and the handles drawn again. (The boxes report the change, which
   settings_command lets pass: it takes a box's number only on Enter or on
   leaving it.) */
static void range_show(void)
{
    int which;
    char text[32];
    for (which = 0; which < 2; which++)
        if (gui.range_box[which]) {
            snprintf(text, sizeof text, "%g", *range_value(&app.s, which));
            SetWindowTextA(gui.range_box[which], text);
            SendMessageW(gui.range_box[which], EM_SETMODIFY, FALSE, 0);
        }
    if (gui.range)
        InvalidateRect(gui.range, NULL, FALSE);
}

/* One handle to v, pushing the other along if it would cross. */
static void range_set(int which, double v)
{
    Settings s = app.s;
    *range_value(&s, which) = v;
    if (s.ripple_max < s.ripple_min)
        *range_value(&s, !which) = v;
    settings_tidy(&s);
    if (memcmp(&s, &app.s, sizeof s)) {
        app.s = s;
        save_and_apply();
        range_show();
    }
}

/* A number typed in one of the boxes, taken on Enter or on leaving it,
   as if its handle were moved there. Anything else puts back what was
   there. */
static void range_typed(int which)
{
    wchar_t text[32], *end;
    double v;
    if (!SendMessageW(gui.range_box[which], EM_GETMODIFY, 0, 0))
        return;
    GetWindowTextW(gui.range_box[which], text, 32);
    decimal_point(text);
    v = wcstod(text, &end);
    while (*end == L' ')
        end++;
    if (end != text && !*end)
        range_set(which, v);
    range_show();
}

static void range_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    RECT r, thumb, channel, travel, part;
    HDC dc = BeginPaint(hwnd, &ps), mem;
    HBITMAP bmp, old;
    HTHEME theme = OpenThemeData(hwnd, L"TRACKBAR");
    int tw, th, which, focused = GetFocus() == hwnd;
    GetClientRect(hwnd, &r);
    mem = CreateCompatibleDC(dc);                /* drawn off screen: no flicker */
    bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
    old = SelectObject(mem, bmp);
    FillRect(mem, &r, GetSysColorBrush(COLOR_BTNFACE));
    range_metrics(&thumb, &channel, &travel);
    tw = thumb.right - thumb.left, th = thumb.bottom - thumb.top;
    part = channel;
    if (theme)
        DrawThemeBackground(theme, mem, TKP_TRACK, TRS_NORMAL, &part, NULL);
    else
        DrawEdge(mem, &part, EDGE_SUNKEN, BF_RECT);
    part.left = range_x(app.s.ripple_min, &travel);    /* the span between them */
    part.right = range_x(app.s.ripple_max, &travel) + 1;
    InflateRect(&part, 0, -1);
    FillRect(mem, &part, GetSysColorBrush(COLOR_HIGHLIGHT));
    for (which = 0; which < 2; which++) {
        int x = range_x(*range_value(&app.s, which), &travel);
        SetRect(&part, x - tw / 2, thumb.top, x - tw / 2 + tw, thumb.top + th);
        if (theme)
            DrawThemeBackground(theme, mem, TKP_THUMBBOTTOM,
                                gui.range_held == which ? TUBS_PRESSED
                                : focused && gui.range_focus == which ? TUBS_FOCUSED
                                : TUBS_NORMAL, &part, NULL);
        else
            DrawFrameControl(mem, &part, DFC_BUTTON, DFCS_BUTTONPUSH
                             | (gui.range_held == which ? DFCS_PUSHED : 0));
    }
    if (focused && !(SendMessageW(GetParent(hwnd), WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
        DrawFocusRect(mem, &r);
    BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    if (theme)
        CloseThemeData(theme);
    EndPaint(hwnd, &ps);
}

/* Drag either handle; the arrow keys, Page Up and Down, Home and End move
   the one in focus, and Space changes which that is. */
static LRESULT CALLBACK range_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RECT thumb, channel, travel;
    if (msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE) {
        range_metrics(&thumb, &channel, &travel);
        if (msg == WM_LBUTTONDOWN) {             /* the nearer handle; if level, by side */
            int x = (short)LOWORD(lp), lo = range_x(app.s.ripple_min, &travel);
            int hi = range_x(app.s.ripple_max, &travel);
            gui.range_held = abs(x - lo) < abs(x - hi) || (lo == hi && x < lo) ? 0 : 1;
            gui.range_focus = gui.range_held;
            SetFocus(hwnd);
            SetCapture(hwnd);
        }
        if (gui.range_held >= 0 && GetCapture() == hwnd)
            range_set(gui.range_held, range_at((short)LOWORD(lp), &travel));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    switch (msg) {
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (GetCapture() == hwnd)
            ReleaseCapture();
        gui.range_held = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN: {
        double v = *range_value(&app.s, gui.range_focus);
        const Field *f = &FIELDS[settings_fields_find("ripple_gap_min_s")];
        switch (wp) {
        case VK_LEFT: case VK_DOWN: v -= 1; break;
        case VK_RIGHT: case VK_UP: v += 1; break;
        case VK_PRIOR: v += 5; break;
        case VK_NEXT: v -= 5; break;
        case VK_HOME: v = f->lo; break;
        case VK_END: v = f->hi; break;
        case VK_SPACE: gui.range_focus = !gui.range_focus; break;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (wp != VK_SPACE)
            range_set(gui.range_focus, v);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        range_paint(hwnd);
        return 0;
    case WM_DESTROY:
        gui.range = NULL;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* The phosphor set to a preset's name or to "#RRGGBB". */
static void phosphor_set(const char *name)
{
    memset(app.s.phosphor, 0, sizeof app.s.phosphor);   /* so equal reads equal */
    snprintf(app.s.phosphor, sizeof app.s.phosphor, "%s", name);
    save_and_apply();
}

static void pick_colour(void)
{
    CHOOSECOLORW cc;
    unsigned char ph[3][3];
    phosphor_colours(app.s.phosphor, ph);
    memset(&cc, 0, sizeof cc);
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = gui.win;
    cc.rgbResult = RGB(ph[0][0], ph[0][1], ph[0][2]);
    cc.lpCustColors = gui.custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&cc)) {
        char hex[8];
        snprintf(hex, sizeof hex, "#%02X%02X%02X", GetRValue(cc.rgbResult),
                 GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
        phosphor_set(hex);
    }
    phosphor_fill();
}

/* Presets: the Look and Animation settings (the fields from phosphor to
   afterglow_s) under a name, one file each in a presets folder beside the
   program, in the settings file's own format. */
static void preset_part(size_t *first, size_t *end)
{
    *first = (size_t)settings_fields_find("phosphor");
    *end = (size_t)settings_fields_find("afterglow_s") + 1;
}

/* The preset's file, from the name typed, as UTF-8 beside the program;
   0 if the name will not do as a file name. */
static int preset_file(const wchar_t *name, char *out, size_t len)
{
    static const wchar_t *devices[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM", L"LPT" };
    char utf8[200];
    size_t n = wcslen(name), stem = wcscspn(name, L"."), k;
    if (!n || n > 60 || wcspbrk(name, L"\\/:*?\"<>|") || name[0] == L'.' || name[n - 1] == L'.')
        return 0;
    for (k = 0; k < 6; k++)        /* Windows keeps CON, NUL, COM1 and the like for devices */
        if (!_wcsnicmp(name, devices[k], 3)
            && (stem == 3 || (k >= 4 && stem == 4 && name[3] >= L'0' && name[3] <= L'9')))
            return 0;
    WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, NULL, NULL);
    snprintf(out, len, "presets\\%s.json", utf8);
    return 1;
}

static void preset_name(wchar_t *name, int len)
{
    wchar_t *a = name, *b;
    GetWindowTextW(GetDlgItem(gui.win, IDC_PRESET), name, len);
    while (*a == L' ')
        a++;
    memmove(name, a, (wcslen(a) + 1) * sizeof *a);
    for (b = name + wcslen(name); b > name && b[-1] == L' '; b--)
        b[-1] = 0;
}

/* The preset that is always there, the Look and Animation defaults, by
   its name in the language in use. */
static const wchar_t *default_preset(void)
{
    return T(L"Default");
}

static int by_name(const void *a, const void *b)
{
    return _wcsicmp((const wchar_t *)a, (const wchar_t *)b);
}

/* Default first, then the saved presets in name order. */
static void preset_list(const wchar_t *select)
{
    static wchar_t names[256][MAX_PATH];
    HWND c = GetDlgItem(gui.win, IDC_PRESET);
    wchar_t pattern[LONG_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, i;
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)default_preset());
    beside("presets\\*.json", pattern, LONG_PATH);
    if ((h = FindFirstFileW(pattern, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *dot = wcsrchr(fd.cFileName, L'.');
            if (dot)
                *dot = 0;
            if (n < 256 && _wcsicmp(fd.cFileName, default_preset())
                && _wcsicmp(fd.cFileName, L"Default"))
                wcscpy(names[n++], fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    qsort(names, (size_t)n, sizeof names[0], by_name);
    for (i = 0; i < n; i++)
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)names[i]);
    SetWindowTextW(c, select);
}

static int ask(const wchar_t *fmt, const wchar_t *name)
{
    wchar_t text[300];
    _snwprintf(text, 300, fmt, name);
    text[299] = 0;
    return MessageBoxW(gui.win, text, L"" NAME, MB_YESNO | MB_ICONQUESTION) == IDYES;
}

static void preset_save(void)
{
    wchar_t name[100], dir[LONG_PATH];
    char file[260];
    size_t first, end;
    preset_name(name, 100);
    if (!_wcsicmp(name, default_preset()) || !_wcsicmp(name, L"Default")) {
        MessageBoxW(gui.win, T(L"Default is built in and always puts the Look and Animation "
                               L"settings back to how the program comes. Save under another "
                               L"name."),
                    L"" NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!preset_file(name, file, sizeof file)) {
        MessageBoxW(gui.win, T(L"Type a name for the preset first: up to 60 characters, "
                               L"without \\ / : * ? \" < > or |, not starting or ending with a "
                               L"full stop, and not a name Windows keeps for devices, such as "
                               L"CON, NUL or COM1."),
                    L"" NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (mtime_beside(file) && !ask(T(L"Replace the preset \"%ls\"?"), name))
        return;
    beside("presets", dir, LONG_PATH);
    CreateDirectoryW(dir, NULL);
    preset_part(&first, &end);
    if (!write_beside(file, &app.s, first, end)) {
        log_note("could not save the preset %s", file);
        MessageBoxW(gui.win, T(L"The preset could not be saved."), L"" NAME,
                    MB_OK | MB_ICONWARNING);
        return;
    }
    preset_list(name);
}

/* The settings in use with the preset's part put in place; only that part
   is taken, whatever else the file holds. 0 if the preset cannot be read. */
static int preset_read(const wchar_t *name, Settings *s)
{
    char file[260], *text;
    FILE *f;
    Settings from = app.s;
    size_t first, end, i;
    if (!_wcsicmp(name, default_preset()))
        settings_defaults(&from);
    else {
        if (!preset_file(name, file, sizeof file) || !(f = open_beside(file, "rb")))
            return 0;
        text = read_all(f);
        fclose(f);
        if (!text)
            return 0;
        settings_parse(&from, text);
        free(text);
    }
    *s = app.s;
    preset_part(&first, &end);
    for (i = first; i < end; i++) {
        const Field *fl = &FIELDS[i];
        memcpy((char *)s + fl->off, (char *)&from + fl->off, field_size(fl));
    }
    settings_tidy(s);
    return 1;
}

static void preset_load(const wchar_t *name)
{
    Settings s;
    if (!preset_read(name, &s))
        return;
    if (memcmp(&s, &app.s, sizeof s)) {
        app.s = s;
        save_and_apply();
    }
    gui_fill();
}

/* Whether the presets' parts match, numbers as a preset file holds them. */
static int preset_same(const Settings *a, const Settings *b)
{
    size_t first, end, i;
    preset_part(&first, &end);
    for (i = first; i < end; i++) {
        const Field *fl = &FIELDS[i];
        const char *x = (const char *)a + fl->off, *y = (const char *)b + fl->off;
        char p[32], q[32];
        if (fl->type != T_STR) {
            snprintf(p, sizeof p, "%.10g", *(const double *)x);
            snprintf(q, sizeof q, "%.10g", *(const double *)y);
            x = p;
            y = q;
        }
        if (strcmp(x, y))
            return 0;
    }
    return 1;
}

/* Shows the preset whose Look and Animation settings are the ones in use:
   Default, else the first saved one that matches, else none. */
static void preset_show_current(void)
{
    HWND c = GetDlgItem(gui.win, IDC_PRESET);
    int i, n = (int)SendMessageW(c, CB_GETCOUNT, 0, 0);
    wchar_t name[MAX_PATH];
    Settings s;
    for (i = 0; i < n; i++)
        if (SendMessageW(c, CB_GETLBTEXTLEN, i, 0) < MAX_PATH) {
            SendMessageW(c, CB_GETLBTEXT, i, (LPARAM)name);
            if (preset_read(name, &s) && preset_same(&s, &app.s)) {
                SendMessageW(c, CB_SETCURSEL, i, 0);
                return;
            }
        }
    SetWindowTextW(c, L"");
}

static void preset_delete(void)
{
    wchar_t name[100], path[LONG_PATH];
    char file[260];
    preset_name(name, 100);
    if (!_wcsicmp(name, default_preset())) {
        MessageBoxW(gui.win, T(L"Default is built in and cannot be deleted."), L"" NAME,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!preset_file(name, file, sizeof file) || !mtime_beside(file))
        return;
    if (!ask(T(L"Delete the preset \"%ls\"?"), name))
        return;
    beside(file, path, LONG_PATH);
    DeleteFileW(path);
    preset_list(L"");
    preset_show_current();
}

/* Hidden rows typed beyond what the panel can hide, given the other box
   and the header, drop to the most it can: so the box says what is used. */
static void hidden_rows_fit(const char *key)
{
    int top = !strcmp(key, "visible_top"), cap, other, most;
    double *v;
    if (!top && strcmp(key, "visible_bottom"))
        return;
    cap = hidden_cap(&app.s);
    other = (int)(top ? app.s.visible_bottom : app.s.visible_top);
    most = cap - (other < cap ? other : cap);
    v = top ? &app.s.visible_top : &app.s.visible_bottom;
    if (*v > most) {
        *v = most;
        save_and_apply();
    }
}

static void settings_build(void);
static void settings_unbuild(void);
static void settings_relayout(UINT screen_dpi, const RECT *work, int x, int y);
static void work_area(HMONITOR screen, RECT *work);

static void settings_status(void)
{
    if (gui.win)
        SetWindowTextW(gui.win, app.save_failed
                       ? T(L"System Monitor Display - Settings (NOT SAVED: the settings "
                           L"file cannot be written; see the log)")
                       : T(L"System Monitor Display - Settings"));
}

/* The language changed: build the window again in it, once the change
   that did it is over. */
static void settings_relabel(void)
{
    if (gui.win)
        PostMessageW(gui.win, WM_RELANG, 0, 0);
}

/* The settings file was edited by hand: show it, and do not let the
   window write back what it showed before. */
static void settings_window_reload(void)
{
    if (gui.win) {
        gui_fill();
        revert_state();
    }
}

/* The controls are gone: nothing is left pointing at them. */
static void gui_forget(void)
{
    memset(gui.ctl, 0, sizeof gui.ctl);
    memset(gui.kind, 0, sizeof gui.kind);
    memset(gui.readout, 0, sizeof gui.readout);
    gui.range_box[0] = gui.range_box[1] = NULL;
}

/* The field whose slider's number box c is; -1 if c is not one. The id
   alone does not say: Windows gives the text part of every drop-down the
   id 1001, which is among the number boxes' own. */
static int readout_field(HWND c)
{
    int i = GetDlgCtrlID(c) - IDC_READOUT;
    return c && i >= 0 && i < (int)NFIELDS && gui.readout[i] == c ? i : -1;
}

/* A command from a control or the keyboard: the control's id, what it
   reports, and the control itself (none for Enter and Esc). */
static void settings_command(HWND hwnd, int id, int code, HWND ctl)
{
    int field = id - IDC_FIELD, typed = readout_field(GetFocus());
    if (gui.filling)
        return;
    if (id == IDOK && typed >= 0) {               /* Enter in a slider's number */
        readout_typed(typed);
        SendMessageW(gui.readout[typed], EM_SETSEL, 0, -1);
    } else if (id == IDOK && (GetFocus() == gui.range_box[0]
                              || GetFocus() == gui.range_box[1])) {   /* and in the gaps' */
        range_typed(GetFocus() == gui.range_box[1]);
        SendMessageW(GetFocus(), EM_SETSEL, 0, -1);
    } else if (id == IDC_GAP || id == IDC_GAP_MAX) {
        if (code == EN_KILLFOCUS)
            range_typed(id == IDC_GAP_MAX);
    } else if (readout_field(ctl) >= 0) {
        if (code == EN_KILLFOCUS)
            readout_typed(readout_field(ctl));
    } else if (id == IDOK && GetParent(GetFocus()) == GetDlgItem(hwnd, IDC_PRESET)) {
        preset_save();                            /* Enter in the preset name */
    } else if (id == IDC_CLOSE || id == IDOK) {   /* OK, or Enter */
        DestroyWindow(hwnd);
    } else if (id == IDCANCEL) {                  /* Cancel, or Esc */
        revert();
        DestroyWindow(hwnd);
    } else if (id == IDC_REVERT) {
        revert();
        preset_show_current();
    } else if (id == IDC_PRESET_SAVE) {
        preset_save();
    } else if (id == IDC_PRESET_DELETE) {
        preset_delete();
    } else if (id == IDC_PRESET && code == CBN_SELCHANGE) {
        int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        wchar_t name[100];
        if (sel >= 0 && SendMessageW(ctl, CB_GETLBTEXTLEN, sel, 0) < 100) {
            SendMessageW(ctl, CB_GETLBTEXT, sel, (LPARAM)name);
            preset_load(name);
        }
    } else if (id == IDC_FOLDER && code == STN_CLICKED) {
        ShellExecuteW(hwnd, L"open", data_folder(), NULL, NULL, SW_SHOWNORMAL);
    } else if (id == IDC_COLOUR && code == BN_CLICKED) {
        pick_colour();
    } else if (id == IDC_PHOSPHOR && code == CBN_SELCHANGE) {
        int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)NPHOSPHORS)
            phosphor_set(PHOSPHORS[sel].name);
        else
            pick_colour();                        /* Custom... */
    } else if (id == IDC_DEGAUSS && code == BN_CLICKED) {
        degauss();
    } else if (id == IDC_STARTUP && code == BN_CLICKED) {
        startup_set(SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED);
        SendMessageW(ctl, BM_SETCHECK, startup_on() ? BST_CHECKED : BST_UNCHECKED, 0);
        revert_state();
    } else if (field < 0 || field >= (int)NFIELDS) {
        return;                                   /* not a setting's control */
    } else if ((code == EN_KILLFOCUS || code == CBN_KILLFOCUS)
               && (gui.kind[field] == K_EDIT || gui.kind[field] == K_COMBO)) {
        /* typed junk, or a value out of range, shows what is used */
        gui.pending = 0;
        gui_apply();
        hidden_rows_fit(FIELDS[field].key);
        gui.filling = 1;
        gui_show((size_t)field);
        gui.filling = 0;
    } else if (!gui.pending && (code == EN_CHANGE || code == BN_CLICKED
                                || code == CBN_SELCHANGE || code == CBN_EDITCHANGE)) {
        /* Applied once the control has settled: a list's new text is not
           in place yet while it reports the change. */
        gui.pending = 1;
        PostMessageW(hwnd, WM_APPLY, 0, 0);
    }
}

static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        settings_command(hwnd, LOWORD(wp), HIWORD(wp), (HWND)lp);
        return 0;
    case WM_NOTIFY: {                       /* the GitHub link */
        const NMHDR *n = (const NMHDR *)lp;
        if (n->idFrom == IDC_LINK && (n->code == NM_CLICK || n->code == NM_RETURN))
            ShellExecuteW(hwnd, L"open", WIDE(HOME_PAGE), NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }
    case WM_HSCROLL:
        if (lp && !gui.filling)
            slider_moved((HWND)lp);
        return 0;
    case WM_APPLY:
        gui.pending = 0;
        gui_apply();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_RELANG: {
        int lang_field = settings_fields_find("language");
        gui.filling = 1;
        settings_unbuild();
        settings_build();
        preset_list(L"");
        preset_show_current();
        gui_fill();
        revert_state();
        settings_status();
        if (gui.ctl[lang_field])
            SetFocus(gui.ctl[lang_field]);  /* where the change was made */
        return 0;
    }
    case WM_DPICHANGED: {                   /* moved to a screen with other scaling */
        const RECT *to = (const RECT *)lp;
        RECT work;
        work_area(MonitorFromRect(to, MONITOR_DEFAULTTONEAREST), &work);
        settings_relayout(HIWORD(wp), &work, to->left, to->top);
        return 0;
    }
    case WM_DESTROY:
        gui.win = NULL;
        gui_forget();
        DeleteObject(gui.font);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static Seen gui_seen;                /* HWiNFO's readings, for the lists */

static UINT window_dpi(HWND w)
{
    typedef UINT(WINAPI * ForWindow)(HWND);
    typedef UINT(WINAPI * ForSystem)(void);
    HMODULE u = GetModuleHandleW(L"user32");
    ForWindow for_window = (ForWindow)(void *)GetProcAddress(u, "GetDpiForWindow");
    ForSystem for_system = (ForSystem)(void *)GetProcAddress(u, "GetDpiForSystem");
    UINT dpi = for_window ? for_window(w) : for_system ? for_system() : 96;
    return dpi ? dpi : 96;
}

/* The room a screen has free of the taskbar. */
static void work_area(HMONITOR screen, RECT *work)
{
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(screen, &mi))
        *work = mi.rcWork;
    else
        SystemParametersInfoW(SPI_GETWORKAREA, 0, work, 0);
}

/* r, a client area, grown to the window's outer size on a screen of the
   given scaling. */
static void window_rect(RECT *r, UINT screen_dpi)
{
    typedef BOOL(WINAPI * AdjustForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
    AdjustForDpi adjust = (AdjustForDpi)(void *)GetProcAddress(GetModuleHandleW(L"user32"),
                                                                "AdjustWindowRectExForDpi");
    DWORD style = (DWORD)GetWindowLongPtrW(gui.win, GWL_STYLE);
    DWORD ex = (DWORD)GetWindowLongPtrW(gui.win, GWL_EXSTYLE);
    if (adjust)
        adjust(r, style, FALSE, ex, screen_dpi);
    else
        AdjustWindowRectEx(r, style, FALSE, ex);
}

/* The scale the window is laid out at, into gui.dpi: its screen's own, or
   less if the window would then be bigger than the room the screen has
   free, as on a 1080p screen at 125 % or more. Never below three quarters
   of the screen's own, where the text gets too small to read; on a screen
   too small even for that, the window's foot is out of reach, and Enter
   and Esc still close it. */
static void settings_scale(UINT screen_dpi, const RECT *work)
{
    RECT frame = { 0, 0, 0, 0 };
    int tall, wide, fit, least = (int)screen_dpi * 3 / 4;
    window_rect(&frame, screen_dpi);
    tall = ((work->bottom - work->top) - (frame.bottom - frame.top)) * 96 / CLIENT_H;
    wide = ((work->right - work->left) - (frame.right - frame.left)) * 96 / CLIENT_W;
    fit = tall < wide ? tall : wide;
    gui.screen_dpi = (int)screen_dpi;
    gui.dpi = fit >= (int)screen_dpi ? (int)screen_dpi : fit > least ? fit : least;
}

/* The window's outer size for its CLIENT_W x CLIENT_H layout at gui.dpi. */
static void settings_size(int *w, int *h)
{
    RECT r = { 0, 0, 0, 0 };
    r.right = S(CLIENT_W), r.bottom = S(CLIENT_H);
    window_rect(&r, (UINT)gui.screen_dpi);
    *w = r.right - r.left, *h = r.bottom - r.top;
}

static BOOL CALLBACK destroy_child(HWND c, LPARAM unused)
{
    (void)unused;
    DestroyWindow(c);
    return TRUE;
}

/* The settings window: everything in the settings file, with a tooltip on
   every control, at gui.dpi and in the language in use. A change takes
   effect, and is saved, as it is made. */
static void settings_build(void)
{
    static const wchar_t *rows[10] = {
        N_(L"Title"), N_(L"HWiNFO reading"), N_(L"Unit"), N_(L"Decimal places"),
        N_(L"Bar starts at"), N_(L"Bar full at"), N_(L"Alarm at or above"),
        N_(L"Alarm clears below"), N_(L"Blink at or above"), N_(L"Alarm after (s)")
    };
    static const char *keys[10] = { "title", "reading", "unit", "decimals", "bar_min",
                                    "bar_max", "alarm_on", "alarm_off", "blink_at",
                                    "alarm_hold_s" };
    static const wchar_t *heads[3] = { N_(L"Box 1 (left)"), N_(L"Box 2 (middle)"),
                                       N_(L"Box 3 (right)") };
    static const wchar_t *UNITS[] = { L"%", L"°C", L"°F", L"W", L"V", L"A", L"MHz", L"GHz",
                                      L"RPM", L"MB", L"GB", L"MB/s", L"FPS", L"ms" };
    Seen *seen = &gui_seen;
    HWND c;
    int b, k, i, y;
    gui.font = CreateFontW(-MulDiv(9, gui.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                           !strcmp(lang.tag, "ja") ? L"Yu Gothic UI"   /* Windows' own UI fonts */
                           : !strcmp(lang.tag, "zh-Hans") ? L"Microsoft YaHei UI" : L"Segoe UI");
    gui.tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, gui.win,
                               NULL, GetModuleHandleW(NULL), NULL);
    SendMessageW(gui.tips, TTM_SETMAXTIPWIDTH, 0, S(360));

    /* The boxes: one column each, a row per setting. */
    group(T(L"Boxes"), 12, 8, 976, 318);
    for (b = 0; b < 3; b++)
        label(T(heads[b]), 140 + b * 278, 26, 268);
    for (k = 0; k < 10; k++) {
        label(T(rows[k]), 22, 50 + k * ROW, 116);
        for (b = 0; b < 3; b++) {
            char key[32];
            snprintf(key, sizeof key, "box%d_%s", b + 1, keys[k]);
            c = control(key, k == 1 || k == 2 ? K_COMBO : k == 3 ? K_LIST : K_EDIT,
                        140 + b * 278, 50 + k * ROW, k == 3 ? 96 : 268, NULL);
            if (k == 3) {
                SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"0");
                SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"1");
                SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"2");
            }
            if (k == 0)
                SendMessageW(c, EM_LIMITTEXT, TITLE_MAX, 0);
            if (k == 2) {                   /* common units; any other can be typed */
                for (i = 0; i < (int)(sizeof UNITS / sizeof UNITS[0]); i++)
                    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)UNITS[i]);
                SendMessageW(c, CB_LIMITTEXT, UNIT_MAX, 0);
            }
            if (k == 1)
                for (i = 0; i < seen->n; i++) {
                    char name[300];
                    wchar_t w[300];
                    if (!seen_name(seen, i, name, sizeof name))
                        continue;
                    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 300);
                    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)w);
                }
        }
    }

    /* Look: a slider for every number */
    group(T(L"Look"), COL1, MID_Y, COL_W, MID_H);
    y = MID_Y + 22;
    label(T(L"Phosphor colour"), COL1 + 10, y, 128);
    c = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, COL1 + FIELD_X, y,
             LIST_W, 200, IDC_PHOSPHOR);
    {   /* in PHOSPHORS' order, then Custom */
        static const wchar_t *const names[NPHOSPHORS] = { N_(L"Fallout"), N_(L"Green"),
                                                          N_(L"Amber"), N_(L"White"),
                                                          N_(L"Blue") };
        for (i = 0; i < (int)NPHOSPHORS; i++)
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(names[i]));
    }
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(L"Custom..."));
    tip(c, T(L"Screen colour: a preset, or Custom to pick any colour"));
    y += ROW;
    c = make(L"BUTTON", T(L"Pick colour..."), WS_TABSTOP | BS_PUSHBUTTON, 0, COL1 + FIELD_X, y,
             LIST_W, 24, IDC_COLOUR);
    tip(c, T(L"Choose any colour for the phosphor"));
    y += ROW;
    slider(T(L"Curvature"), "curvature", COL1, &y);
    slider(T(L"Glow"), "glow", COL1, &y);
    slider(T(L"Brightness"), "brightness", COL1, &y);
    slider(T(L"Contrast"), "contrast", COL1, &y);
    slider(T(L"Scanline height (px)"), "scanline_px", COL1, &y);
    directions(row(T(L"Scanline drift"), "scanline_drift_direction", K_LIST, COL1, &y, LIST_W));
    slider(T(L"Scanline drift (px/s)"), "scanline_drift_speed", COL1, &y);
    slider(T(L"Afterglow (s)"), "afterglow_s", COL1, &y);
    c = row(T(L"Frames per second"), "fps", K_LIST, COL1, &y, LIST_W);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"30");
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"15");

    /* Animation: each effect's tick box with its direction beside it */
    group(T(L"Animation"), COL2, MID_Y, COL_W, MID_H);
    y = MID_Y + 22;
    control("hum_bar", K_CHECK, COL2 + 10, y, LABEL_W, T(L"Hum bar"));
    directions(control("hum_bar_direction", K_LIST, COL2 + FIELD_X, y, LIST_W, NULL));
    y += ROW;
    slider(T(L"Hum bar period (s)"), "hum_bar_period_s", COL2, &y);
    slider(T(L"Hum bar intensity"), "hum_bar_intensity", COL2, &y);
    slider(T(L"Hum bar thickness (px)"), "hum_bar_thickness", COL2, &y);
    control("ripple", K_CHECK, COL2 + 10, y, LABEL_W, T(L"Ripple"));
    directions(control("ripple_direction", K_LIST, COL2 + FIELD_X, y, LIST_W, NULL));
    y += ROW;
    slider(T(L"Ripple amplitude (%)"), "ripple_amplitude_pct", COL2, &y);
    slider(T(L"Ripple frequency (Hz)"), "ripple_frequency_hz", COL2, &y);
    slider(T(L"Ripple thickness (px)"), "ripple_thickness", COL2, &y);
    slider(T(L"Ripple drift (Hz)"), "ripple_drift_hz", COL2, &y);
    slider(T(L"Ripple period (s)"), "ripple_period_s", COL2, &y);
    slider(T(L"Ripple static"), "ripple_static", COL2, &y);
    /* the gaps: one slider with two handles, where the others have one,
       in the same place, with the shortest's box to its left (in the end
       of the label column) and the longest's in the numbers' column */
    label(T(L"Ripple gap (s)"), COL2 + 10, y, LABEL_W - 44);
    gui.range_box[0] = make(L"EDIT", L"", WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL,
                            WS_EX_CLIENTEDGE, COL2 + FIELD_X - 44, y, 36, 23, IDC_GAP);
    tip(gui.range_box[0], tip_for("ripple_gap_min_s"));
    gui.range = make(RANGE_CLASS, L"", WS_TABSTOP, 0, COL2 + FIELD_X - 4, y, 96, 24, -1);
    gui.range_held = -1;
    tip(gui.range, T(L"Drag either handle: the shortest and longest time between ripples. "
                     L"The arrow keys move the one in focus, and Space changes which"));
    gui.range_box[1] = make(L"EDIT", L"", WS_TABSTOP | ES_RIGHT | ES_AUTOHSCROLL,
                            WS_EX_CLIENTEDGE, COL2 + FIELD_X + 112, y, 36, 23, IDC_GAP_MAX);
    tip(gui.range_box[1], tip_for("ripple_gap_max_s"));
    y += ROW;

    /* General */
    group(T(L"General"), COL3, MID_Y, COL_W, MID_H);
    y = MID_Y + 22;
    c = row(T(L"Language"), "language", K_LIST, COL3, &y, 148);
    gui.nlangs = languages(gui.langs, MAX_LANGS);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)T(L"Automatic"));
    for (i = 0; i < gui.nlangs; i++)
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)gui.langs[i].native);
    row(T(L"Hidden rows at top"), "visible_top", K_EDIT, COL3, &y, 60);
    row(T(L"Hidden rows at bottom"), "visible_bottom", K_EDIT, COL3, &y, 60);
    slider(T(L"Smoothing (s)"), "smoothing_s", COL3, &y);
    slider(T(L"No reading after (s)"), "stale_after_s", COL3, &y);
    c = make(L"BUTTON", T(L"Start with Windows"), WS_TABSTOP | BS_AUTOCHECKBOX, 0, COL3 + 10, y,
             300, 23, IDC_STARTUP);
    tip(c, T(L"Starts the panel when you sign in to Windows, from wherever this program is. "
             L"Move the program and it follows the next time it starts"));
    y += ROW;
    {   /* where the settings, presets and log are; long paths lose their middle */
        wchar_t folder[LONG_PATH], text[LONG_PATH + 64];
        size_t n;
        wcscpy(folder, data_folder());
        n = wcslen(folder);
        if (n > 3 && folder[n - 1] == L'\\')
            folder[n - 1] = 0;
        label(T(L"Settings, presets and log are in:"), COL3 + 10, y + 2, 300);
        c = make(L"STATIC", folder, SS_LEFT | SS_PATHELLIPSIS | SS_NOTIFY, 0, COL3 + 10, y + 24,
                 300, 20, IDC_FOLDER);
        _snwprintf(text, LONG_PATH + 64, T(L"%ls\nClick to open the folder in Explorer"), folder);
        text[LONG_PATH + 63] = 0;
        tip(c, text);
    }
    label(L"" NAME L" " VERSION_W, COL3 + 10, y + 54, 300);
    label(T(L"\u00A9 2026 Jared Cabot. MIT licence."), COL3 + 10, y + 74, 300);
    c = make(L"SysLink", L"<a>github.com/JaredCabot/system-monitor-display</a>", WS_TABSTOP, 0,
             COL3 + 10, y + 96, 300, 20, IDC_LINK);
    tip(c, T(L"Opens the project's page on GitHub: the source, the README and new versions"));
    c = make(L"BUTTON", T(L"Degauss"), WS_TABSTOP | BS_PUSHBUTTON, 0, COL3 + 10, y + 124, 140, 28,
             IDC_DEGAUSS);
    tip(c, T(L"Clears the tube's magnetism, as the button on a colour CRT did: the picture "
             L"shakes and the coil thunks and buzzes for two seconds"));

    /* Header */
    group(T(L"Header"), COL1, LOW_Y, 648, LOW_H);
    control("header", K_CHECK, 22, LOW_Y + 20, 300, T(L"Show a header above the boxes"));
    label(T(L"Line 1"), 22, LOW_Y + 48, 116);
    c = control("header_line1", K_EDIT, 140, LOW_Y + 48, 420, NULL);
    SendMessageW(c, EM_LIMITTEXT, HEADER_MAX, 0);
    label(T(L"Line 2"), 22, LOW_Y + 74, 116);
    c = control("header_line2", K_EDIT, 140, LOW_Y + 74, 420, NULL);
    SendMessageW(c, EM_LIMITTEXT, HEADER_MAX, 0);

    /* Presets */
    group(T(L"Presets"), COL3, LOW_Y, COL_W, LOW_H);
    c = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL,
             0, COL3 + 10, LOW_Y + 22, 300, 300, IDC_PRESET);
    tip(c, T(L"Pick a preset to put its Look and Animation settings in place; Default is how "
             L"the program comes. To keep the current ones, type a name and click Save"));
    tip(GetWindow(c, GW_CHILD), T(L"Pick a preset to put its Look and Animation settings in "
                                  L"place; Default is how the program comes. To keep the "
                                  L"current ones, type a name and click Save"));
    c = make(L"BUTTON", T(L"Save"), WS_TABSTOP | BS_PUSHBUTTON, 0, COL3 + 10, LOW_Y + 60, 100, 28,
             IDC_PRESET_SAVE);
    tip(c, T(L"Saves the Look and Animation settings as a preset under the name above"));
    c = make(L"BUTTON", T(L"Delete"), WS_TABSTOP | BS_PUSHBUTTON, 0, COL3 + 118, LOW_Y + 60, 100,
             28, IDC_PRESET_DELETE);
    tip(c, T(L"Deletes the preset named above"));

    c = make(L"BUTTON", T(L"Revert"), WS_TABSTOP | BS_PUSHBUTTON, 0, 12, BUTTONS_Y, 100, 28,
             IDC_REVERT);
    tip(c, T(L"Puts every setting back as it was when this window opened"));
    c = make(L"BUTTON", T(L"OK"), WS_TABSTOP | BS_DEFPUSHBUTTON, 0, CLIENT_W - 220, BUTTONS_Y,
             100, 28, IDC_CLOSE);
    tip(c, T(L"Keeps the changes, which are already saved, and closes the window"));
    c = make(L"BUTTON", T(L"Cancel"), WS_TABSTOP | BS_PUSHBUTTON, 0, CLIENT_W - 112, BUTTONS_Y,
             100, 28, IDCANCEL);
    tip(c, T(L"Puts every setting back as it was when this window opened, and closes the "
             L"window"));
}

/* Taken down to be built again at another scale or in another language. */
static void settings_unbuild(void)
{
    EnumChildWindows(gui.win, destroy_child, 0);
    DestroyWindow(gui.tips);
    DeleteObject(gui.font);
    gui_forget();
}

/* The window laid out again for a screen of the given scaling and free
   room, with its top-left corner at (x, y). */
static void settings_relayout(UINT screen_dpi, const RECT *work, int x, int y)
{
    wchar_t preset[100];
    int w, h;
    GetWindowTextW(GetDlgItem(gui.win, IDC_PRESET), preset, 100);
    gui.filling = 1;
    settings_unbuild();
    settings_scale(screen_dpi, work);
    settings_size(&w, &h);
    SetWindowPos(gui.win, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    settings_build();
    preset_list(preset);
    gui_fill();
    revert_state();
}

/* The settings window, built but not yet shown; 0 if it could not be.
   It is laid out for the scaling of the screen it is on (smaller if it
   would not fit there), in the middle of that screen, and laid out afresh
   if it is moved to a screen with another scaling (see WM_DPICHANGED), so
   the text stays sharp and the right size. */
static int settings_create(void)
{
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    wchar_t mark[4];
    RECT work;
    int w, h, x, y;
    gui.win = CreateWindowExW(WS_EX_CONTROLPARENT, SETTINGS_CLASS,
                              L"" NAME, style, CW_USEDEFAULT, CW_USEDEFAULT,
                              100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!gui.win) {
        log_note("could not open the settings window (error %lu)", GetLastError());
        return 0;
    }
    work_area(MonitorFromWindow(gui.win, MONITOR_DEFAULTTONEAREST), &work);
    settings_scale(window_dpi(gui.win), &work);
    settings_size(&w, &h);
    x = work.left + ((work.right - work.left) - w) / 2;
    y = work.top + ((work.bottom - work.top) - h) / 2;
    SetWindowPos(gui.win, NULL, x > work.left ? x : work.left, y > work.top ? y : work.top, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);    /* its title bar always in reach */
    gui.decimal = GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, mark, 4) == 2 ? mark[0]
                                                                                     : L'.';
    collect_labels(&gui_seen);
    settings_build();
    gui.opened = app.s;
    gui.opened_startup = startup_on();
    preset_list(L"");
    preset_show_current();
    gui_fill();
    revert_state();
    settings_status();
    return 1;
}

static void settings_open(void)
{
    if (!gui.win && !settings_create())
        return;
    ShowWindow(gui.win, SW_SHOWNORMAL);
    SetForegroundWindow(gui.win);
}

/* ------------------------------------------------------ the panel window */
static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        tick();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        blit(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        if (hwnd == app.win && app.placed)
            return MA_NOACTIVATE;           /* a click never takes focus */
        break;
    case WM_DISPLAYCHANGE:
        app.house_at = 0;                   /* look for the screen now */
        return 0;
    case WM_ENDSESSION:                     /* signing out or shutting down */
        if (wp && app.save_due)
            settings_save();
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU
            || LOWORD(lp) == WM_LBUTTONUP)
            tray_menu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_EDIT)
            settings_open();
        else if (LOWORD(wp) == ID_PAUSE) {
            app.mon.paused = !app.mon.paused;
            timer_set();
        } else if (LOWORD(wp) == ID_DEGAUSS)
            degauss();
        else if (LOWORD(wp) == ID_QUIT)
            PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == app.taskbar_created && msg) {  /* Explorer restarted */
            tray_add();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Everything the panel needs before its window: the readings, the
   picture, the icon and the two window classes. */
static void app_prepare(double now)
{
    WNDCLASSW wc, sc;
    INITCOMMONCONTROLSEX icc;
    app.last = now, app.poll_at = 0, app.house_at = now + 2;
    lang_use(app.s.language);
    monitor_init(&app.mon, &app.s);
    panel_configure(&app.panel, &app.s);
    app.icon = make_icon(&app.s);
    snprintf(app.icon_colour, sizeof app.icon_colour, "%s", app.s.phosphor);
    memset(&app.bmi, 0, sizeof app.bmi);
    app.bmi.bmiHeader.biSize = sizeof app.bmi.bmiHeader;
    app.bmi.bmiHeader.biWidth = W, app.bmi.bmiHeader.biHeight = -H;  /* top down */
    app.bmi.bmiHeader.biPlanes = 1, app.bmi.bmiHeader.biBitCount = 32;
    app.bmi.bmiHeader.biCompression = BI_RGB;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = app.icon;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = PANEL_CLASS;
    RegisterClassW(&wc);
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);
    sc = wc;
    sc.lpfnWndProc = settings_proc;
    sc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    sc.lpszClassName = SETTINGS_CLASS;
    RegisterClassW(&sc);
    sc.lpfnWndProc = range_proc;
    sc.hIcon = NULL;
    sc.hbrBackground = NULL;
    sc.lpszClassName = RANGE_CLASS;
    RegisterClassW(&sc);
}

/* The borderless panel window: off the taskbar and Alt+Tab, and never
   takes the keyboard focus. */
static HWND panel_window(int x, int y)
{
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, PANEL_CLASS, L"" NAME,
                           WS_POPUP, x, y, W, H, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static int run_panel(void)
{
    MSG msg;
    HANDLE mutex;
    dpi_aware();
    mutex = CreateMutexW(NULL, FALSE, L"Local\\" SLUG);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* One copy at a time. Started again, say from Explorer while the
           tray icon is out of sight: open the running copy's settings,
           and let it come to the front. */
        HWND running = FindWindowW(PANEL_CLASS, L"" NAME);
        DWORD pid = 0;
        if (running) {
            GetWindowThreadProcessId(running, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(running, WM_COMMAND, ID_EDIT, 0);
        }
        return 0;
    }
    (void)mutex;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    startup_follow();
    srand((unsigned)time(NULL));
    settings_load(&app.s, SLUG ".json");
    app.mtime = mtime_beside(SLUG ".json");
    app_prepare(now_s());
    app.placed = find_screen(&app.place);
    app.have_place = app.placed;
    if (app.placed)
        app.win = panel_window(app.place.x, app.place.y);
    else {
        Monitors m;
        char list[512] = "";
        int i;
        RECT r = { 0, 0, W, H };
        monitors(&m);
        for (i = 0; i < m.n; i++)
            snprintf(list + strlen(list), sizeof list - strlen(list), " (%ld, %ld, %ld, %ld)",
                     m.r[i].left, m.r[i].top, m.r[i].right - m.r[i].left,
                     m.r[i].bottom - m.r[i].top);
        log_note("no %dx%d monitor found; opened as a window. Monitors:%s", W, H, list);
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        app.win = CreateWindowExW(0, PANEL_CLASS, L"" NAME, WS_OVERLAPPEDWINDOW, 100, 100,
                                  r.right - r.left, r.bottom - r.top, NULL, NULL,
                                  GetModuleHandleW(NULL), NULL);
    }
    app.tray_owner = CreateWindowExW(WS_EX_TOOLWINDOW, PANEL_CLASS, L"", WS_POPUP, 0, 0, 0, 0,
                                     NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!app.win || !app.tray_owner) {
        wchar_t text[300];
        log_note("could not create the window (error %lu)", GetLastError());
        _snwprintf(text, 300, T(L"Could not create the panel window. Details are in %ls "
                                L"beside the program."), L"" SLUG L".log");
        text[299] = 0;
        MessageBoxW(NULL, text, L"" NAME, MB_ICONERROR);
        return 1;
    }
    app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    tray_add();
    ShowWindow(app.win, app.placed ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    timer_set();
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (gui.win && IsDialogMessageW(gui.win, &msg))
            continue;                       /* Tab, Enter and Esc in settings */
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (app.save_due)
        settings_save();
    Shell_NotifyIconW(NIM_DELETE, &app.nid);
    return 0;
}

/* ------------------------------------------------------------- diagnose */
/* Print what the panel can see: monitors and HWiNFO's readings. */
static int diagnose(void)
{
    static Seen seen;
    Monitors m;
    Settings s;
    Want w;
    int i;
    size_t r;
    dpi_aware();
    monitors(&m);
    printf("Monitors (left, top, width, height):\n");
    for (i = 0; i < m.n; i++) {
        long width = m.r[i].right - m.r[i].left, height = m.r[i].bottom - m.r[i].top;
        printf("  (%ld, %ld, %ld, %ld)%s\n", m.r[i].left, m.r[i].top, width, height,
               width == W && height == H ? "   <- the panel's screen" : "");
    }
    printf("\nSettings, presets and log:\n  %ls\n", data_folder());
    settings_load(&s, SLUG ".json");
    for (r = 0; r < NSOURCES; r++) {
        uint64_t stamp;
        printf("\n%s:\n", SOURCES[r].name);
        w.labels[0] = s.box[0].reading, w.labels[1] = s.box[1].reading;
        w.labels[2] = s.box[2].reading;
        w.have[0] = w.have[1] = w.have[2] = 0;
        seen.n = 0;
        if (!SOURCES[r].each(seen_visit, &seen, &stamp)) {
            printf("  nothing - not switched on in HWiNFO, or HWiNFO is not running\n");
            continue;
        }
        SOURCES[r].each(want_visit, &w, &stamp);
        qsort(seen.r, seen.n, sizeof seen.r[0], by_label);
        for (i = 0; i < 3; i++) {
            int k, times = 0;
            if (!w.have[i]) {
                printf("  %-22s NOT FOUND\n", w.labels[i]);
                continue;
            }
            printf("  %-22s found, %g\n", w.labels[i], w.value[i]);
            for (k = 0; k < seen.n; k++)
                times += !strcmp(seen.r[k].label, w.labels[i]);
            if (times > 1)
                printf("    on %d sensors; the panel takes the first. To pick one, use"
                       " the sensor name too, as listed below.\n", times);
        }
        printf("  (%d readings in all)\n  Readings that might suit:\n", seen.n);
        for (i = 0; i < seen.n; i++) {
            const char *l = seen.r[i].label;
            char name[300];
            if ((strstr(l, "Usage") || strstr(l, "Load") || strstr(l, "Tctl")
                 || strstr(l, "Tdie") || strstr(l, "Utilization"))
                && seen_name(&seen, i, name, sizeof name))
                printf("    %s\n", name);
        }
    }
    return 0;
}

/* A GUI program has no console of its own: borrow the one it was started
   from, so --diagnose and the rest can print. Output sent to a file or a
   pipe ("--diagnose > diagnose.txt") stays there. */
static void attach_console(void)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN)
        return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        printf("\n");
    }
}
#endif

/* ============================================================== start */
int main(int argc, char **argv)
{
    int i;
    tables();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "--selftest")
            || !strcmp(argv[i], "--preview") || !strcmp(argv[i], "--diagnose")
            || !strcmp(argv[i], "--check-translations")) {
#ifdef _WIN32
            attach_console();
#endif
            break;
        }
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) {
            printf("%s %s\n", NAME, VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--selftest"))
            return selftest();
        if (!strcmp(argv[i], "--check-translations")) {
#ifdef _WIN32
            return check_translations();
#else
            printf("--check-translations needs Windows.\n");
            return 1;
#endif
        }
        if (!strcmp(argv[i], "--diagnose")) {
#ifdef _WIN32
            return diagnose();
#else
            printf("--diagnose needs Windows.\n");
            return 1;
#endif
        }
        if (!strcmp(argv[i], "--preview")) {
            const char *path = i + 1 < argc ? argv[i + 1] : "preview.bmp";
            double values[3] = { 47, 61, 82 };
            int has[3] = { 1, 1, 1 }, k, alarm = 0;
            int j;
            for (j = i + 2, k = 0; j < argc && k < 3; j++, k++) {
                if (!strcmp(argv[j], "-"))
                    has[k] = 0, values[k] = 0;
                else if (argv[j][0] != '-' || isdigit((unsigned char)argv[j][1]))
                    has[k] = 1, values[k] = atof(argv[j]);
                else
                    break;
            }
            for (k = 1; k < argc; k++)
                alarm |= !strcmp(argv[k], "--alarm");
            preview(path, values, has, alarm);
            return 0;
        }
    }
#ifdef _WIN32
    return run_panel();
#else
    printf("The panel itself runs on Windows; try --preview here.\n");
    return 1;
#endif
}
