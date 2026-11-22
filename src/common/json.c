// A small JSON reader: see json.h. Recursive descent over the text, nodes appended in order.

#include "common/json.h"

#include <string.h>

typedef struct {
    const char *p, *end;
    MsJson *nodes;
    int count, max, depth;
} Reader;

static void skip_space(Reader *r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r'))
        ++r->p;
}

static int new_node(Reader *r, MsJsonType type) {
    if (r->count >= r->max)
        return -1;
    MsJson *n = &r->nodes[r->count];
    memset(n, 0, sizeof *n);
    n->type = type;
    n->first = n->next = -1;
    return r->count++;
}

// A string's characters (after its opening quote): returns 0 at its end, -1 if unterminated.
static int read_string(Reader *r, const char **start, int *length) {
    *start = r->p;
    while (r->p < r->end && *r->p != '"') {
        if (*r->p == '\\' && r->p + 1 < r->end)
            ++r->p;
        ++r->p;
    }
    if (r->p >= r->end)
        return -1;
    *length = (int)(r->p - *start);
    ++r->p; // the closing quote
    return 0;
}

static int read_value(Reader *r);

// The children of an array or object (after its opening bracket), linked to node `parent`.
static int read_children(Reader *r, int parent, char close, int object) {
    int last = -1;
    skip_space(r);
    if (r->p < r->end && *r->p == close) {
        ++r->p;
        return 0;
    }
    for (;;) {
        const char *key = NULL;
        int key_length = 0;
        skip_space(r);
        if (object) {
            if (r->p >= r->end || *r->p != '"')
                return -1;
            ++r->p;
            if (read_string(r, &key, &key_length) != 0)
                return -1;
            skip_space(r);
            if (r->p >= r->end || *r->p != ':')
                return -1;
            ++r->p;
        }
        const int child = read_value(r);
        if (child < 0)
            return -1;
        r->nodes[child].key = key;
        r->nodes[child].key_length = key_length;
        if (last < 0)
            r->nodes[parent].first = child;
        else
            r->nodes[last].next = child;
        last = child;
        ++r->nodes[parent].count;
        skip_space(r);
        if (r->p < r->end && *r->p == ',') {
            ++r->p;
            continue;
        }
        if (r->p < r->end && *r->p == close) {
            ++r->p;
            return 0;
        }
        return -1;
    }
}

static int read_value(Reader *r) {
    skip_space(r);
    if (r->p >= r->end || r->depth > 256)
        return -1;
    const char c = *r->p;
    int n;
    if (c == '{' || c == '[') {
        ++r->p;
        n = new_node(r, c == '{' ? MS_JSON_OBJECT : MS_JSON_ARRAY);
        ++r->depth;
        if (n < 0 || read_children(r, n, c == '{' ? '}' : ']', c == '{') != 0)
            return -1;
        --r->depth;
        return n;
    }
    if (c == '"') {
        ++r->p;
        n = new_node(r, MS_JSON_STRING);
        if (n < 0 || read_string(r, &r->nodes[n].text, &r->nodes[n].length) != 0)
            return -1;
        return n;
    }
    static const struct {
        const char *word;
        MsJsonType type;
        double value;
    } words[] = {{"true", MS_JSON_BOOL, 1}, {"false", MS_JSON_BOOL, 0}, {"null", MS_JSON_NULL, 0}};
    for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i) {
        const size_t len = strlen(words[i].word);
        if ((size_t)(r->end - r->p) >= len && strncmp(r->p, words[i].word, len) == 0) {
            r->p += len;
            n = new_node(r, words[i].type);
            if (n >= 0)
                r->nodes[n].number = words[i].value;
            return n;
        }
    }
    // A number, read by hand (strtod depends on the locale's decimal point).
    double sign = 1, value = 0, scale = 1;
    if (r->p < r->end && *r->p == '-')
        sign = -1, ++r->p;
    const char *digits = r->p;
    while (r->p < r->end && *r->p >= '0' && *r->p <= '9')
        value = value * 10 + (*r->p++ - '0');
    if (r->p < r->end && *r->p == '.') {
        ++r->p;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
            scale /= 10;
            value += (*r->p++ - '0') * scale;
        }
    }
    if (r->p == digits)
        return -1;
    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
        ++r->p;
        int e_sign = 1, e = 0;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-'))
            e_sign = *r->p++ == '-' ? -1 : 1;
        if (r->p >= r->end || *r->p < '0' || *r->p > '9')
            return -1;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9' && e < 400)
            e = e * 10 + (*r->p++ - '0');
        for (int i = 0; i < e; ++i)
            value = e_sign > 0 ? value * 10 : value / 10;
    }
    n = new_node(r, MS_JSON_NUMBER);
    if (n >= 0)
        r->nodes[n].number = sign * value;
    return n;
}

int ms_json_parse(const char *text, size_t length, MsJson *nodes, int max) {
    Reader r = {text, text + length, nodes, 0, max, 0};
    if (!text || read_value(&r) != 0)
        return -1;
    skip_space(&r);
    return r.p == r.end ? r.count : -1;
}

int ms_json_member(const MsJson *nodes, int object, const char *key) {
    if (object < 0 || nodes[object].type != MS_JSON_OBJECT)
        return -1;
    const size_t len = strlen(key);
    for (int c = nodes[object].first; c >= 0; c = nodes[c].next)
        if ((size_t)nodes[c].key_length == len && strncmp(nodes[c].key, key, len) == 0)
            return c;
    return -1;
}

double ms_json_number(const MsJson *nodes, int index, double fallback) {
    if (index < 0 || (nodes[index].type != MS_JSON_NUMBER && nodes[index].type != MS_JSON_BOOL))
        return fallback;
    return nodes[index].number;
}

double ms_json_get(const MsJson *nodes, int object, const char *key, double fallback) {
    return ms_json_number(nodes, ms_json_member(nodes, object, key), fallback);
}

int ms_json_is(const MsJson *nodes, int object, const char *key, const char *value) {
    const int m = ms_json_member(nodes, object, key);
    return m >= 0 && nodes[m].type == MS_JSON_STRING && (size_t)nodes[m].length == strlen(value) &&
           strncmp(nodes[m].text, value, (size_t)nodes[m].length) == 0;
}
