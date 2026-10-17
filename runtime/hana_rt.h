#ifndef HANA_RT_H
#define HANA_RT_H

/* Runtime support library linked into every HANA program. Compiled code
 * calls these functions; their signatures are part of the code generator's
 * contract (see src/codegen/codegen.cpp). */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hana_rt_print_str(const char* data, int64_t length);
void hana_rt_print_i64(int64_t value);
void hana_rt_print_u64(uint64_t value);
void hana_rt_print_f64(double value);
void hana_rt_print_f32(float value);
void hana_rt_print_bool(bool value);
/* Prints a Unicode scalar value as UTF-8. */
void hana_rt_print_char(uint32_t code_point);

/* Formats the shortest decimal text that reads back as the same value, and
 * adds ".0" when the text would otherwise look like an integer. Writes at
 * most size bytes including the terminator and returns the text length. */
int hana_rt_format_f64(double value, char* buffer, int size);
int hana_rt_format_f32(float value, char* buffer, int size);

/* A HANA 'str': UTF-8 bytes, not terminated. A capacity of 0 means the
 * bytes are not owned (a literal) and are never freed; otherwise they were
 * allocated by these functions. The layout matches the code generator's
 * { ptr, i64, i64 }. */
typedef struct hana_str {
    char* data;
    int64_t length;
    int64_t capacity;
} hana_str;

/* Appends bytes or the text of a value to *s, which becomes owned. */
void hana_rt_str_push(hana_str* s, const char* data, int64_t length);
void hana_rt_str_push_i64(hana_str* s, int64_t value);
void hana_rt_str_push_u64(hana_str* s, uint64_t value);
void hana_rt_str_push_f64(hana_str* s, double value);
void hana_rt_str_push_f32(hana_str* s, float value);
void hana_rt_str_push_bool(hana_str* s, bool value);
void hana_rt_str_push_char(hana_str* s, uint32_t code_point);

/* *out = a + b, a new owned string. */
void hana_rt_str_concat(hana_str* out, const char* a, int64_t a_length, const char* b,
                        int64_t b_length);
/* *out = an owned copy of the bytes. */
void hana_rt_str_clone(hana_str* out, const char* data, int64_t length);
/* Compares bytes like memcmp, then by length: <0, 0 or >0. */
int32_t hana_rt_str_compare(const char* a, int64_t a_length, const char* b, int64_t b_length);
/* Frees owned bytes; does nothing for a capacity of 0. */
void hana_rt_str_free(char* data, int64_t capacity);

/* Flushes standard output, prints "panic: <message>" to standard error and
 * aborts the process. */
_Noreturn void hana_rt_panic(const char* data, int64_t length);

#ifdef __cplusplus
}
#endif

#endif
