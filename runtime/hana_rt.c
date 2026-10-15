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

_Noreturn void hana_rt_panic(const char* data, int64_t length)
{
    fflush(stdout);
    fputs("panic: ", stderr);
    fwrite(data, 1, (size_t)length, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}
