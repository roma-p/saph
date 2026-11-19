// MillSaph's presets (preset.h): finding them, the preset window, applying one.

#include "common/preset.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/json.h"
#include "common/log.h"
#include "common/process.h"

#ifdef _WIN32
#include <windows.h>
#define SEPARATOR "\\"
#else
#include <sys/stat.h>
#define SEPARATOR "/"
#endif

static int is_dir(const char *path) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int ms_presets_dir(const char *folder, char *out, size_t size) {
    char dir[1024];
    if (!folder || !ms_module_dir(dir, sizeof dir))
        return 0;
    // the bundle's Contents/Resources (the plugin in Contents/<platform>/ or Contents/MacOS/),
    // else beside the plugin (After Effects on Windows)
    static const char *const places[] = {".." SEPARATOR "Resources" SEPARATOR "presets", "presets"};
    for (size_t i = 0; i < sizeof places / sizeof places[0]; ++i) {
        const int n = snprintf(out, size, "%s%s%s%s", dir, places[i], SEPARATOR, folder);
        if (n > 0 && (size_t)n + 1 < size && is_dir(out)) {
            strcat(out, SEPARATOR);
            return 1;
        }
    }
    return 0;
}

// A JSON string's characters with its escapes resolved (\uXXXX as UTF-8) into `out` (at
// least length + 1 bytes).
static void unescape(const char *s, int length, char *out) {
    char *o = out;
    for (int i = 0; i < length; ++i) {
        if (s[i] != '\\' || i + 1 >= length) {
            *o++ = s[i];
            continue;
        }
        const char c = s[++i];
        switch (c) {
        case 'n':
            *o++ = '\n';
            break;
        case 't':
            *o++ = '\t';
            break;
        case 'r':
            *o++ = '\r';
            break;
        case 'b':
            *o++ = '\b';
            break;
        case 'f':
            *o++ = '\f';
            break;
        case 'u': {
            unsigned code = 0;
            for (int k = 0; k < 4 && i + 1 < length && isxdigit((unsigned char)s[i + 1]); ++k) {
                const int h = tolower((unsigned char)s[++i]);
                code = code * 16 + (unsigned)(isdigit(h) ? h - '0' : h - 'a' + 10);
            }
            if (code < 0x80) {
                *o++ = (char)code;
            } else if (code < 0x800) {
                *o++ = (char)(0xC0 | (code >> 6));
                *o++ = (char)(0x80 | (code & 0x3F));
            } else { // (surrogate pairs: each half as it is)
                *o++ = (char)(0xE0 | (code >> 12));
                *o++ = (char)(0x80 | ((code >> 6) & 0x3F));
                *o++ = (char)(0x80 | (code & 0x3F));
            }
            break;
        }
        default:
            *o++ = c;
            break; // \" \\ \/
        }
    }
    *o = 0;
}

// The effect's parameter named `name`, NULL if none.
static const MsParam *param_named(const MsEffect *e, const char *name, int length) {
    for (int i = 0; i < e->param_count; ++i) {
        const char *n = e->params[i].name;
        if (n && (int)strlen(n) == length && strncmp(n, name, (size_t)length) == 0)
            return &e->params[i];
    }
    return NULL;
}

// Sets `p` (with its RGB parts or its POINT's y) to `v` (count values; POINT: fractions of
// the frame). Returns 1 when done.
static int set_param(MsHost *host, const MsParam *p, const double *v, int count) {
    char part[96];
    switch (p->type) {
    case MS_PARAM_RGB:
        if (count < 3)
            return 0;
        snprintf(part, sizeof part, "%s_green", p->name);
        host->set_value(host, p->name, v[0]);
        host->set_value(host, part, v[1]);
        snprintf(part, sizeof part, "%s_blue", p->name);
        return host->set_value(host, part, v[2]);
    case MS_PARAM_POINT: {
        double frame[2];
        if (count < 2 || !host->frame_size || !host->frame_size(host, frame))
            return 0;
        snprintf(part, sizeof part, "%s_y", p->name);
        host->set_value(host, p->name, v[0] * frame[0]);
        return host->set_value(host, part, v[1] * frame[1]);
    }
    case MS_PARAM_CHOICE: {
        const int last = p->entry_count > 0 ? p->entry_count - 1 : 0;
        const double i = v[0] < 0 ? 0 : v[0] > last ? last : v[0];
        return count >= 1 && host->set_value(host, p->name, i);
    }
    case MS_PARAM_DOUBLE:
    case MS_PARAM_WIDTH:
    case MS_PARAM_HEIGHT:
    case MS_PARAM_ANGLE:
    case MS_PARAM_BOOL:
        return count >= 1 && host->set_value(host, p->name, v[0]);
    default:
        return 0;
    }
}

int ms_apply_preset(const MsEffect *e, MsHost *host, const char *text, size_t length, char *err,
                    size_t err_size) {
    const int max = (int)(length / 2) + 16; // (a node takes two characters at least)
    MsJson *nodes = malloc(sizeof(MsJson) * (size_t)max);
    char *buffer = malloc(length + 1);
    int ok = nodes && buffer && ms_json_parse(text, length, nodes, max) > 0 &&
             nodes[0].type == MS_JSON_OBJECT;
    const int values = ok ? ms_json_member(nodes, 0, "params") : -1;
    if (values < 0 || nodes[values].type != MS_JSON_OBJECT) {
        snprintf(err, err_size, "not a preset");
        free(nodes);
        free(buffer);
        return 0;
    }
    // every parameter at its default
    for (int i = 0; i < e->param_count; ++i) {
        const MsParam *p = &e->params[i];
        if (p->type == MS_PARAM_STRING) {
            host->set_text(host, p->name, p->def_text ? p->def_text : "");
        } else if (p->type == MS_PARAM_RGB && i + 2 < e->param_count) {
            const double v[3] = {p[0].def, p[1].def, p[2].def};
            set_param(host, p, v, 3);
        } else if (p->type == MS_PARAM_POINT && i + 1 < e->param_count) {
            const double v[2] = {p[0].def, p[1].def};
            set_param(host, p, v, 2);
        } else {
            set_param(host, p, &p->def, 1);
        }
    }
    // then the preset's
    int skipped = 0;
    for (int k = nodes[values].first; k >= 0; k = nodes[k].next) {
        const MsJson *n = &nodes[k];
        const MsParam *p = param_named(e, n->key, n->key_length);
        double v[3] = {0, 0, 0};
        int count = 0;
        if (!p) {
            ++skipped;
        } else if (n->type == MS_JSON_STRING && p->type == MS_PARAM_STRING) {
            unescape(n->text, n->length, buffer);
            host->set_text(host, p->name, buffer);
        } else if (n->type == MS_JSON_NUMBER || n->type == MS_JSON_BOOL) {
            v[0] = n->number;
            set_param(host, p, v, 1);
        } else if (n->type == MS_JSON_ARRAY) {
            for (int c = n->first; c >= 0 && count < 3; c = nodes[c].next)
                v[count++] = ms_json_number(nodes, c, 0.0);
            set_param(host, p, v, count);
        }
    }
    if (skipped)
        ms_log("%s preset: %d settings this version does not have", e->label, skipped);
    free(nodes);
    free(buffer);
    return 1;
}

// A file's contents (NUL-terminated) into a new buffer the caller frees, NULL on failure.
static char *read_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    long size = -1;
    char *text = NULL;
    if (f && fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0)
        text = malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, f) == (size_t)size) {
        text[size] = 0;
        *length = (size_t)size;
    } else {
        free(text);
        text = NULL;
    }
    if (f)
        fclose(f);
    return text;
}

int ms_load_preset(const MsEffect *e, MsHost *host, char *err, size_t err_size) {
    char dir[1024], browser[1200], folder[1024], out[1024], picked[2048] = "";
#ifdef _WIN32
    const char *exe = "millsaph_preset_browser.exe";
#else
    const char *exe = "millsaph_preset_browser";
#endif
    if (!ms_module_dir(dir, sizeof dir) || !ms_presets_dir(e->presets, folder, sizeof folder)) {
        snprintf(err, err_size, "cannot find the presets");
        return 0;
    }
    snprintf(browser, sizeof browser, "%s%s", dir, exe);
    if (!ms_temp_file(".txt", out, sizeof out)) {
        snprintf(err, err_size, "cannot make a temporary file");
        return 0;
    }
    const char *argv[] = {browser, "--dir", folder, "--title", e->label, "--out", out, NULL};
    const int code = ms_run_program(argv);
    size_t length = 0;
    if (code == 0) { // a preset picked: its file's path in `out`
        char *path = read_file(out, &length);
        if (path) {
            snprintf(picked, sizeof picked, "%s", path);
            picked[strcspn(picked, "\r\n")] = 0;
        }
        free(path);
    }
    remove(out);
    if (code < 0) {
        snprintf(err, err_size, "cannot run the preset window (%s)", browser);
        return 0;
    }
    if (code != 0 || !picked[0]) // cancelled
        return 1;
    char *text = read_file(picked, &length);
    const int ok = text ? ms_apply_preset(e, host, text, length, err, err_size) : 0;
    if (!text)
        snprintf(err, err_size, "cannot read %s", picked);
    free(text);
    return ok;
}
