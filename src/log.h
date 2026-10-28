/*
 * Copyright (c) 2026 Luca Machiedo
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <stdbool.h>
#include <stdio.h>

#include "runara/runara.h"

struct rn_log_t {
  FILE *stream;
  bool  quiet;
  bool  verbose;

  rn_log_write_fn_t write_cb;
};

void rn_log_write_cb_default(FILE *stream, enum rn_log_level_t level,
                             const char *subsystem, const char *function,
                             const char *file, int line, const char *fmt,
                             va_list args);

void rn_log_emit(const struct rn_log_t *log, enum rn_log_level_t level,
                 const char *subsystem, const char *function, const char *file,
                 int line, const char *fmt, ...);

char *rn_log_get_filepath(void);

#define RN_LOG_AT(log, level, ...)                                             \
  rn_log_emit(&(log), (level), RN_SUBSYS_NAME, __func__, __FILE__, __LINE__,   \
              __VA_ARGS__)

#define RN_TRACE(log, ...) RN_LOG_AT(log, RN_LL_TRACE, __VA_ARGS__)

#define RN_INFO(log, ...) RN_LOG_AT(log, RN_LL_INFO, __VA_ARGS__)

#define RN_WARN(log, ...) RN_LOG_AT(log, RN_LL_WARN, __VA_ARGS__)

#define RN_ERROR(log, ...) RN_LOG_AT(log, RN_LL_ERR, __VA_ARGS__)

#define RN_FATAL(log, ...) RN_LOG_AT(log, RN_LL_FATAL, __VA_ARGS__)

#define RN_LOG_HEADLESS(level, ...)                                            \
  rn_log_emit(NULL, (level), RN_SUBSYS_NAME, __func__, __FILE__, __LINE__,     \
              __VA_ARGS__)

#define RN_TRACE_HEADLESS(...) RN_LOG_HEADLESS(RN_LL_TRACE, __VA_ARGS__)

#define RN_INFO_HEADLESS(...) RN_LOG_HEADLESS(RN_LL_INFO, __VA_ARGS__)

#define RN_WARN_HEADLESS(...) RN_LOG_HEADLESS(RN_LL_WARN, __VA_ARGS__)

#define RN_ERROR_HEADLESS(...) RN_LOG_HEADLESS(RN_LL_ERR, __VA_ARGS__)

#define RN_FATAL_HEADLESS(...) RN_LOG_HEADLESS(RN_LL_FATAL, __VA_ARGS__)
