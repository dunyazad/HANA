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

/* Flushes standard output, prints "panic: <message>" to standard error and
 * aborts the process. */
_Noreturn void hana_rt_panic(const char* data, int64_t length);

#ifdef __cplusplus
}
#endif

#endif
