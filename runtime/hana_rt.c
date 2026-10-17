#include "hana_rt.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hana_rt_print_str(const char* data, int64_t length)
{
    if (length > 0) {
        fwrite(data, 1, (size_t)length, stdout);
    }
}

void hana_rt_print_i64(int64_t value)
{
    printf("%" PRId64, value);
}

void hana_rt_print_u64(uint64_t value)
{
    printf("%" PRIu64, value);
}

void hana_rt_print_bool(bool value)
{
    fputs(value ? "true" : "false", stdout);
}

/* Writes the UTF-8 bytes of a Unicode scalar value; returns their count. */
static size_t encode_utf8(uint32_t code_point, char buffer[4])
{
    size_t length = 0;
    if (code_point < 0x80) {
        buffer[length++] = (char)code_point;
    } else if (code_point < 0x800) {
        buffer[length++] = (char)(0xC0 | (code_point >> 6));
        buffer[length++] = (char)(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        buffer[length++] = (char)(0xE0 | (code_point >> 12));
        buffer[length++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        buffer[length++] = (char)(0x80 | (code_point & 0x3F));
    } else {
        buffer[length++] = (char)(0xF0 | (code_point >> 18));
        buffer[length++] = (char)(0x80 | ((code_point >> 12) & 0x3F));
        buffer[length++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
        buffer[length++] = (char)(0x80 | (code_point & 0x3F));
    }
    return length;
}

void hana_rt_print_char(uint32_t code_point)
{
    char buffer[4];
    const size_t length = encode_utf8(code_point, buffer);
    fwrite(buffer, 1, length, stdout);
}

/* Appends ".0" to finite values printed without a fraction or exponent, so
 * 2.0 prints as "2.0" rather than "2". */
static int add_fraction_if_integral(char* buffer, int length, int size)
{
    for (int i = 0; i < length; ++i) {
        const char c = buffer[i];
        if (c == '.' || c == 'e' || c == 'n' || c == 'i') {
            return length; /* fraction, exponent, nan or inf */
        }
    }
    if (length + 2 < size) {
        buffer[length++] = '.';
        buffer[length++] = '0';
        buffer[length] = '\0';
    }
    return length;
}

int hana_rt_format_f64(double value, char* buffer, int size)
{
    int length = 0;
    for (int precision = 1; precision <= 17; ++precision) {
        length = snprintf(buffer, (size_t)size, "%.*g", precision, value);
        if (value != value || strtod(buffer, NULL) == value) {
            break;
        }
    }
    return add_fraction_if_integral(buffer, length, size);
}

int hana_rt_format_f32(float value, char* buffer, int size)
{
    int length = 0;
    for (int precision = 1; precision <= 9; ++precision) {
        length = snprintf(buffer, (size_t)size, "%.*g", precision, (double)value);
        if (value != value || strtof(buffer, NULL) == value) {
            break;
        }
    }
    return add_fraction_if_integral(buffer, length, size);
}

void hana_rt_print_f64(double value)
{
    char buffer[40];
    const int length = hana_rt_format_f64(value, buffer, (int)sizeof buffer);
    fwrite(buffer, 1, (size_t)length, stdout);
}

void hana_rt_print_f32(float value)
{
    char buffer[40];
    const int length = hana_rt_format_f32(value, buffer, (int)sizeof buffer);
    fwrite(buffer, 1, (size_t)length, stdout);
}

static void out_of_memory(void)
{
    static const char message[] = "out of memory";
    hana_rt_panic(message, (int64_t)(sizeof message - 1));
}

/* Makes room for 'extra' more bytes, copying unowned bytes into a new
 * owned buffer. Capacity grows by doubling. */
static void reserve(hana_str* s, int64_t extra)
{
    const int64_t needed = s->length + extra;
    if (s->capacity > 0 && needed <= s->capacity) {
        return;
    }
    int64_t capacity = s->capacity > 0 ? s->capacity : 16;
    while (capacity < needed) {
        capacity *= 2;
    }
    char* data = NULL;
    if (s->capacity > 0) {
        data = realloc(s->data, (size_t)capacity);
    } else {
        data = malloc((size_t)capacity);
        if (data != NULL && s->length > 0) {
            memcpy(data, s->data, (size_t)s->length);
        }
    }
    if (data == NULL) {
        out_of_memory();
    }
    s->data = data;
    s->capacity = capacity;
}

/* The bytes may be part of *s itself ('s += s'), so their position is
 * found again after the buffer grows. */
void hana_rt_str_push(hana_str* s, const char* data, int64_t length)
{
    const uintptr_t begin = (uintptr_t)s->data;
    const uintptr_t source = (uintptr_t)data;
    const int inside = s->capacity > 0 && source >= begin && source < begin + (uintptr_t)s->length;
    const uintptr_t offset = source - begin;
    reserve(s, length > 0 ? length : 0);
    if (length > 0) {
        memmove(s->data + s->length, inside ? s->data + offset : data, (size_t)length);
        s->length += length;
    }
}

void hana_rt_str_push_i64(hana_str* s, int64_t value)
{
    char buffer[24];
    const int length = snprintf(buffer, sizeof buffer, "%" PRId64, value);
    hana_rt_str_push(s, buffer, length);
}

void hana_rt_str_push_u64(hana_str* s, uint64_t value)
{
    char buffer[24];
    const int length = snprintf(buffer, sizeof buffer, "%" PRIu64, value);
    hana_rt_str_push(s, buffer, length);
}

void hana_rt_str_push_f64(hana_str* s, double value)
{
    char buffer[40];
    const int length = hana_rt_format_f64(value, buffer, (int)sizeof buffer);
    hana_rt_str_push(s, buffer, length);
}

void hana_rt_str_push_f32(hana_str* s, float value)
{
    char buffer[40];
    const int length = hana_rt_format_f32(value, buffer, (int)sizeof buffer);
    hana_rt_str_push(s, buffer, length);
}

void hana_rt_str_push_bool(hana_str* s, bool value)
{
    if (value) {
        hana_rt_str_push(s, "true", 4);
    } else {
        hana_rt_str_push(s, "false", 5);
    }
}

void hana_rt_str_push_char(hana_str* s, uint32_t code_point)
{
    char buffer[4];
    const size_t length = encode_utf8(code_point, buffer);
    hana_rt_str_push(s, buffer, (int64_t)length);
}

void hana_rt_str_concat(hana_str* out, const char* a, int64_t a_length, const char* b,
                        int64_t b_length)
{
    hana_str result = {NULL, 0, 0};
    reserve(&result, a_length + b_length);
    hana_rt_str_push(&result, a, a_length);
    hana_rt_str_push(&result, b, b_length);
    *out = result;
}

void hana_rt_str_clone(hana_str* out, const char* data, int64_t length)
{
    hana_str result = {NULL, 0, 0};
    reserve(&result, length);
    hana_rt_str_push(&result, data, length);
    *out = result;
}

int32_t hana_rt_str_compare(const char* a, int64_t a_length, const char* b, int64_t b_length)
{
    const int64_t common = a_length < b_length ? a_length : b_length;
    const int order = common > 0 ? memcmp(a, b, (size_t)common) : 0;
    if (order != 0) {
        return order < 0 ? -1 : 1;
    }
    if (a_length == b_length) {
        return 0;
    }
    return a_length < b_length ? -1 : 1;
}

void hana_rt_str_free(char* data, int64_t capacity)
{
    if (capacity > 0) {
        free(data);
    }
}

_Noreturn void hana_rt_panic(const char* data, int64_t length)
{
    fflush(stdout);
    fputs("panic: ", stderr);
    fwrite(data, 1, (size_t)length, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}
