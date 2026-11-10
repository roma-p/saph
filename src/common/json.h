// A small JSON reader (RFC 8259) for the settings effects take as text (e.g. a glare style):
// the document as a tree of nodes in a table the caller gives, strings pointing into the
// text (not copied, escapes left as they are). No allocation.

#pragma once

#include <stddef.h>

typedef enum {
    MS_JSON_NULL,
    MS_JSON_BOOL,
    MS_JSON_NUMBER,
    MS_JSON_STRING,
    MS_JSON_ARRAY,
    MS_JSON_OBJECT
} MsJsonType;

typedef struct {
    MsJsonType type;
    double number;       // NUMBER; BOOL: 0 or 1
    const char *text;    // STRING: its characters (without the quotes)
    int length;          // of `text`
    const char *key;     // the member's name in its object (NULL in an array)
    int key_length;
    int first, next;     // ARRAY / OBJECT: first child; the next sibling (-1: none)
    int count;           // ARRAY / OBJECT: number of children
} MsJson;

// Reads `text` (its first `length` bytes) into `nodes` (at most `max`). Returns the number of
// nodes (the root is node 0), or -1 if the text is not JSON or `max` is too small.
int ms_json_parse(const char *text, size_t length, MsJson *nodes, int max);

// The member `key` of object node `object`, -1 if none.
int ms_json_member(const MsJson *nodes, int object, const char *key);

// Node `index`'s number (BOOL: 0 or 1), or `fallback` when it is not one or index < 0.
double ms_json_number(const MsJson *nodes, int index, double fallback);

// Member `key` of `object` as a number, or `fallback`.
double ms_json_get(const MsJson *nodes, int object, const char *key, double fallback);

// 1 when member `key` of `object` is the string `value`.
int ms_json_is(const MsJson *nodes, int object, const char *key, const char *value);
