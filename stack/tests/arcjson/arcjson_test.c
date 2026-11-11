/*
 * lane.arcjson (M2-04): the stack's JSON parser and writer against RFC 8259 edge cases:
 * escapes, surrogate pairs, bad UTF-8, deep nesting, numbers at the limits, trailing
 * garbage, the size limit, and writer output that parses back to the same values. Also the
 * helpers manifests share (M3-02): UTF-8 check, whole-file read, schema id and version checks.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../lane_check.h"
#include "arcjson.h"

#define LIMIT (1u << 20)

/* parses: true if `text` is accepted. */
static bool parses(const char *text) {
    arcjson_arena a;
    arcjson_arena_init(&a);
    bool ok = arcjson_parse(&a, text, strlen(text), LIMIT, NULL) != NULL;
    arcjson_arena_free(&a);
    return ok;
}

/* string_is: `text` parses to a string equal to the `len` bytes of `want`. */
static bool string_is(const char *text, const char *want, size_t len) {
    arcjson_arena a;
    arcjson_arena_init(&a);
    const arcjson_value *v = arcjson_parse(&a, text, strlen(text), LIMIT, NULL);
    bool ok =
        v != NULL && v->type == ARCJSON_STRING && v->u.string.len == len && memcmp(v->u.string.ptr, want, len) == 0;
    arcjson_arena_free(&a);
    return ok;
}

/* number_is: `text` parses to the number `want`. */
static bool number_is(const char *text, double want) {
    arcjson_arena a;
    arcjson_arena_init(&a);
    const arcjson_value *v = arcjson_parse(&a, text, strlen(text), LIMIT, NULL);
    bool ok = v != NULL && v->type == ARCJSON_NUMBER && v->u.number == want;
    arcjson_arena_free(&a);
    return ok;
}

static void test_literals_and_structure(void) {
    lane_check(parses("null") && parses(" true ") && parses("false"), "literals, surrounding whitespace");
    lane_check(parses("{}") && parses("[]") && parses("{\"a\":[1,{\"b\":null}]}"), "empty and nested containers");
    lane_check(!parses("") && !parses("   "), "empty input refused");
    lane_check(!parses("[1,]") && !parses("{\"a\":1,}") && !parses("[1 2]"), "trailing or missing comma refused");
    lane_check(!parses("{a:1}") && !parses("{\"a\" 1}") && !parses("{1:2}"), "bad member syntax refused");
    lane_check(!parses("tru") && !parses("nul") && !parses("True"), "bad literals refused");
    lane_check(!parses("{} x") && !parses("1 2") && !parses("[]]"), "trailing garbage refused");
    lane_check(!parses("[1") && !parses("{\"a\":") && !parses("\"abc"), "truncated input refused");

    arcjson_arena a;
    arcjson_arena_init(&a);
    const char *doc = "{\"name\":\"x\",\"n\":3,\"list\":[1,2,3],\"name\":\"dup\"}";
    const arcjson_value *v = arcjson_parse(&a, doc, strlen(doc), LIMIT, NULL);
    const arcjson_value *list = arcjson_get(v, "list");
    int64_t n = 0;
    lane_check(v != NULL && v->u.children.count == 4, "object member count");
    lane_check(strcmp(arcjson_string(arcjson_get(v, "name")), "x") == 0, "duplicate key: first wins");
    lane_check(arcjson_int64(arcjson_get(v, "n"), &n) && n == 3, "integer member");
    lane_check(list != NULL && list->type == ARCJSON_ARRAY && list->u.children.count == 3 &&
                   list->u.children.first->next->u.number == 2,
               "array elements in order");
    lane_check(arcjson_get(v, "missing") == NULL && arcjson_get(list, "x") == NULL, "missing member, non-object");
    arcjson_arena_free(&a);
}

static void test_strings(void) {
    lane_check(string_is("\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"", "a\"b\\c/d\b\f\n\r\t", 12), "all simple escapes");
    lane_check(string_is("\"\\u00e9\\u20ac\"", "\xC3\xA9\xE2\x82\xAC", 5), "\\u escapes to UTF-8");
    lane_check(string_is("\"\\ud83d\\ude00\"", "\xF0\x9F\x98\x80", 4), "surrogate pair combined");
    lane_check(string_is("\"a\\u0000b\"", "a\0b", 3), "escaped NUL kept, length right");
    lane_check(string_is("\"\xC2\xB0\xC2\xB5\xC2\xB1\"", "\xC2\xB0\xC2\xB5\xC2\xB1", 6), "raw UTF-8 kept");
    lane_check(!parses("\"\\ud83d\"") && !parses("\"\\ud83dx\"") && !parses("\"\\ude00\""), "lone surrogates refused");
    lane_check(!parses("\"\\ud83d\\u0041\""), "high surrogate followed by a non-surrogate refused");
    lane_check(!parses("\"\\x\"") && !parses("\"\\u12\"") && !parses("\"\\u12g4\""), "bad escapes refused");
    lane_check(!parses("\"a\nb\"") && !parses("\"\t\""), "raw control characters refused");
    lane_check(!parses("\"\xC0\xAF\"") && !parses("\"\xE0\x80\xAF\""), "overlong UTF-8 refused");
    lane_check(!parses("\"\xED\xA0\x80\"") && !parses("\"\xF4\x90\x80\x80\""), "UTF-8 surrogate, > U+10FFFF refused");
    lane_check(!parses("\"\x80\"") && !parses("\"\xFF\"") && !parses("\"\xE2\x82\""),
               "stray, invalid, cut bytes refused");
}

static void test_numbers(void) {
    lane_check(number_is("0", 0) && number_is("-0", 0) && number_is("12.5e-1", 1.25), "plain numbers");
    lane_check(number_is("1.7976931348623157e308", 1.7976931348623157e308), "largest double");
    lane_check(number_is("4.9e-324", 4.9e-324) && number_is("1e-400", 0), "smallest double, underflow to 0");
    lane_check(!parses("1e309") && !parses("-1e309"), "overflow refused");
    lane_check(!parses("01") && !parses("-") && !parses("1.") && !parses(".5") && !parses("1e") && !parses("+1"),
               "bad number grammar refused");
    lane_check(!parses("NaN") && !parses("Infinity") && !parses("0x10"), "non-JSON numbers refused");

    arcjson_arena a;
    arcjson_arena_init(&a);
    int64_t n;
    const arcjson_value *v = arcjson_parse(&a, "[9007199254740992, 9007199254740994, 1.5, -3]", 45, LIMIT, NULL);
    const arcjson_value *e = v->u.children.first;
    lane_check(arcjson_int64(e, &n) && n == 9007199254740992LL, "2^53 is an integer");
    lane_check(!arcjson_int64(e->next, &n), "beyond 2^53 is not");
    lane_check(!arcjson_int64(e->next->next, &n), "1.5 is not an integer");
    lane_check(arcjson_int64(e->next->next->next, &n) && n == -3, "negative integer");
    arcjson_arena_free(&a);
}

static void test_limits(void) {
    char deep[2 * ARCJSON_MAX_DEPTH + 3];
    for (int depth = ARCJSON_MAX_DEPTH; depth <= ARCJSON_MAX_DEPTH + 1; depth++) {
        memset(deep, '[', (size_t)depth);
        memset(deep + depth, ']', (size_t)depth);
        deep[2 * depth] = '\0';
        lane_check(parses(deep) == (depth == ARCJSON_MAX_DEPTH),
                   depth == ARCJSON_MAX_DEPTH ? "nesting at the limit accepted" : "nesting over the limit refused");
    }
    arcjson_arena a;
    arcjson_arena_init(&a);
    arcjson_error err;
    lane_check(arcjson_parse(&a, "[1,2,3]", 7, 6, &err) == NULL && strcmp(err.message, "input too large") == 0,
               "size limit enforced");
    lane_check(arcjson_parse(&a, "[1,2,3]", 7, 7, NULL) != NULL, "input exactly at the limit accepted");
    lane_check(arcjson_parse(&a, "[1,x]", 5, LIMIT, &err) == NULL && err.offset == 3,
               "error offset points at the fault");
    lane_check(arcjson_parse(&a, "[1]", 2, LIMIT, NULL) == NULL, "length, not NUL, ends the input");
    arcjson_arena_free(&a);
}

static void test_writer(void) {
    arcjson_writer w;
    arcjson_writer_init(&w);
    arcjson_write_object_begin(&w);
    arcjson_write_key(&w, "s");
    arcjson_write_string(&w, "q\"b\\\n\x01\xC2\xB0");
    arcjson_write_key(&w, "bad");
    arcjson_write_string_n(&w,
                           "a\xFF"
                           "b\xE2\x82",
                           5);
    arcjson_write_key(&w, "nums");
    arcjson_write_array_begin(&w);
    arcjson_write_int(&w, -42);
    arcjson_write_number(&w, 0.1);
    arcjson_write_float(&w, 0.1f);
    arcjson_write_number(&w, NAN);
    arcjson_write_array_end(&w);
    arcjson_write_key(&w, "b");
    arcjson_write_bool(&w, true);
    arcjson_write_key(&w, "n");
    arcjson_write_null(&w);
    arcjson_write_key(&w, "empty");
    arcjson_write_object_begin(&w);
    arcjson_write_object_end(&w);
    arcjson_write_object_end(&w);
    size_t len = 0;
    const char *out = arcjson_writer_result(&w, &len);
    printf("writer: %s\n", out ? out : "(failed)");
    lane_check(out != NULL && strcmp(out, "{\"s\":\"q\\\"b\\\\\\n\\u0001\xC2\xB0\",\"bad\":\"a\xEF\xBF\xBD"
                                          "b\xEF\xBF\xBD\xEF\xBF\xBD\",\"nums\":[-42,0.10000000000000001,"
                                          "0.100000001,null],\"b\":true,\"n\":null,\"empty\":{}}") == 0,
               "writer output exact");

    arcjson_arena a;
    arcjson_arena_init(&a);
    const arcjson_value *v = out ? arcjson_parse(&a, out, len, LIMIT, NULL) : NULL;
    const arcjson_value *nums = arcjson_get(v, "nums");
    lane_check(v != NULL && strcmp(arcjson_string(arcjson_get(v, "s")), "q\"b\\\n\x01\xC2\xB0") == 0,
               "writer output parses back to the same string");
    lane_check(nums != NULL && nums->u.children.first->next->u.number == 0.1 &&
                   (float)nums->u.children.first->next->next->u.number == 0.1f,
               "double and float round-trip");
    arcjson_arena_free(&a);

    arcjson_writer_reset(&w);
    arcjson_write_array_begin(&w);
    lane_check(arcjson_writer_result(&w, NULL) == NULL, "unclosed document has no result");
    arcjson_write_array_end(&w);
    arcjson_write_array_end(&w);
    lane_check(arcjson_writer_result(&w, NULL) == NULL, "closing too often fails");
    arcjson_writer_free(&w);
}

static void test_utf8_complete_len(void) {
    lane_check(arcjson_utf8_complete_len("ab", 2) == 2, "ASCII complete");
    lane_check(arcjson_utf8_complete_len("a\xC2", 2) == 1, "cut 2-byte character held back");
    lane_check(arcjson_utf8_complete_len("a\xF0\x9F\x98", 4) == 1, "cut 4-byte character held back");
    lane_check(arcjson_utf8_complete_len("a\xF0\x9F\x98\x80", 5) == 5, "complete 4-byte character");
    lane_check(arcjson_utf8_complete_len("a\xFF", 2) == 2, "invalid byte not held back");
    lane_check(arcjson_utf8_valid("", 0) && arcjson_utf8_valid("a\xC2\xB0\xF0\x9F\x98\x80", 7), "valid UTF-8");
    lane_check(!arcjson_utf8_valid("a\xC2", 2) && !arcjson_utf8_valid("\xC0\xAF", 2) &&
                   !arcjson_utf8_valid("\xED\xA0\x80", 3),
               "cut, overlong and surrogate UTF-8 refused");
}

/* arcjson_read_file reads a whole file NUL-terminated and refuses missing and oversized ones. */
static void test_read_file(void) {
    char path[256];
    const char *dir = getenv("TMPDIR");
    snprintf(path, sizeof(path), "%s/arcjson_test.%ld.json", dir ? dir : "/tmp", (long)getpid());
    FILE *f = fopen(path, "w");
    lane_check(f != NULL && fputs("{\"a\": 1}", f) >= 0 && fclose(f) == 0, "write a small JSON file");
    size_t len = 0;
    const char *why = NULL;
    char *text = arcjson_read_file(path, 64, &len, &why);
    lane_check(text != NULL && len == 8 && strcmp(text, "{\"a\": 1}") == 0, "read whole, NUL-terminated");
    free(text);
    lane_check(arcjson_read_file(path, 7, &len, &why) == NULL && strstr(why, "size limit") != NULL,
               "file over the limit refused");
    unlink(path);
    lane_check(arcjson_read_file(path, 64, &len, &why) == NULL && why != NULL, "missing file refused");
}

/* The schema id and version checks shared by roles.json and pack manifests. */
static void test_schema_names(void) {
    lane_check(arcjson_is_schema_id("medical-en", 10) && arcjson_is_schema_id("a1", 2), "ids accepted");
    lane_check(!arcjson_is_schema_id("", 0) && !arcjson_is_schema_id("-a", 2) && !arcjson_is_schema_id("Ab", 2) &&
                   !arcjson_is_schema_id("a_b", 3),
               "empty, leading '-', upper case, '_' refused in ids");
    char long_id[66];
    memset(long_id, 'a', sizeof(long_id));
    lane_check(arcjson_is_schema_id(long_id, 64) && !arcjson_is_schema_id(long_id, 65), "ids at most 64 bytes");
    lane_check(arcjson_is_schema_version("1.0.0", 5) && arcjson_is_schema_version("2.1.0-rc.1+b7", 13),
               "versions accepted");
    lane_check(!arcjson_is_schema_version("", 0) && !arcjson_is_schema_version("v1", 2) &&
                   !arcjson_is_schema_version("1/0", 3) && !arcjson_is_schema_version("1 0", 3),
               "empty, leading letter, '/', space refused in versions");
    memset(long_id, '1', sizeof(long_id));
    lane_check(arcjson_is_schema_version(long_id, 64) && !arcjson_is_schema_version(long_id, 65),
               "versions at most 64 bytes");
}

int main(void) {
    test_literals_and_structure();
    test_strings();
    test_numbers();
    test_limits();
    test_writer();
    test_utf8_complete_len();
    test_read_file();
    test_schema_names();
    return lane_check_done("arcjson");
}
