/*
 * ps3recomp - Logging header
 */

#ifndef PS3EMU_LOGGING_H
#define PS3EMU_LOGGING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void ps3_log_lock();
extern void ps3_log_unlock();
extern int ps3_log(char const* const _Format, ...);
extern int ps3_print(char const* const _Format, ...);

#ifdef __cplusplus
}
#endif


#endif /* PS3EMU_LOGGING_H */
