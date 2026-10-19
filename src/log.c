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

#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *level_names[RN_LL_COUNT] = {"TRACE", "INFO", "WARNING",
                                               "ERROR", "FATAL"};

static const char *level_colors[RN_LL_COUNT] = {
    "\033[1;32m", "\033[1;36m", "\033[1;33m", "\033[1;31m", "\033[38;5;88m"};

static bool _log_mkdirs(const char *path);
static void _log_header(FILE *stream, enum rn_log_level_t level);

static bool _log_mkdirs(const char *path) {
  char buf[PATH_MAX];

  int n = snprintf(buf, sizeof(buf), "%s", path);
  if (n < 0 || (size_t)n >= sizeof(buf))
    return false;

  for (char *p = buf + 1; *p; p++) {
    if (*p != '/')
      continue;

    *p = '\0';

    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
      return false;

    *p = '/';
  }

  if (mkdir(buf, 0755) != 0 && errno != EEXIST)
    return false;

  struct stat st;
  return stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
}

static void _log_header(FILE *stream, enum rn_log_level_t level) {
  time_t now = time(NULL);
  struct tm tm = {0};
  char timebuf[9] = "00:00:00";

  if (localtime_r(&now, &tm))
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

  bool colorize = stream == stderr || stream == stdout;

  fprintf(stream,
          "[runara]: %s%s%s: %s%s%s: ", colorize ? level_colors[level] : "",
          level_names[level], colorize ? "\033[0m" : "",
          colorize ? "\033[1;34m" : "", timebuf, colorize ? "\033[0m" : "");
}

void rn_log_write_cb_default(FILE *stream, enum rn_log_level_t level,
                             const char *subsystem, const char *function,
                             const char *file, int line, const char *fmt,
                             va_list args) {
  _log_header(stream, level);

  if (level == RN_LL_INFO) {
    fprintf(stream, "%s: ", subsystem);
  } else if (level == RN_LL_TRACE) {
    fprintf(stream, "%s: %s: ", subsystem, function);
  } else {
    fprintf(stream, "%s: %s (%s:%d): ", subsystem, function, file, line);
  }

  vfprintf(stream, fmt, args);
  fputc('\n', stream);
  fflush(stream);
}

void rn_log_emit(const struct rn_log_t *log, enum rn_log_level_t level,
                 const char *subsystem, const char *function, const char *file,
                 int line, const char *fmt, ...) {
  if (!log || !log->write_cb || level < RN_LL_TRACE || level >= RN_LL_COUNT)
    return;

  bool fatal = level == RN_LL_FATAL;

  if (log && !log->verbose && level == RN_LL_TRACE)
    return;

  FILE *terminal = level >= RN_LL_ERR ? stderr : stdout;
  FILE *logfile = NULL;

  if (log && log->stream && log->stream != stdout && log->stream != stderr)
    logfile = log->stream;

  va_list args;
  va_start(args, fmt);

  if (logfile) {
    va_list copy;
    va_copy(copy, args);

    log->write_cb(logfile, level, subsystem, function, file, line, fmt, copy);

    va_end(copy);
  }

  if (!log || !log->quiet || fatal) {
    FILE *stream = terminal;

    if (log && (log->stream == stdout || log->stream == stderr)) {
      stream = log->stream;

      if (level >= RN_LL_ERR && stream == stdout)
        stream = stderr;
    }

    va_list copy;
    va_copy(copy, args);

    log->write_cb(stream, level, subsystem, function, file, line, fmt, copy);

    va_end(copy);
  }

  va_end(args);

  if (fatal)
    exit(EXIT_FAILURE);
}

char *rn_log_get_filepath(void) {
  static char filepath[PATH_MAX];

  const char *state_home = getenv("XDG_STATE_HOME");
  const char *home = getenv("HOME");

  char fallback[PATH_MAX];

  if (!state_home || !*state_home) {
    if (!home || !*home)
      return NULL;

    int n = snprintf(fallback, sizeof(fallback), "%s/.local/state", home);

    if (n < 0 || (size_t)n >= sizeof(fallback))
      return NULL;

    state_home = fallback;
  }

  char log_dir[PATH_MAX];

  int n = snprintf(log_dir, sizeof(log_dir), "%s/runara/logs", state_home);

  if (n < 0 || (size_t)n >= sizeof(log_dir))
    return NULL;

  if (!_log_mkdirs(log_dir))
    return NULL;

  time_t now = time(NULL);
  struct tm tm = {0};
  char timestamp[32];

  if (!localtime_r(&now, &tm))
    return NULL;

  if (!strftime(timestamp, sizeof(timestamp), "%Y-%m-%d-%H%M", &tm))
    return NULL;

  n = snprintf(filepath, sizeof(filepath), "%s/runara-%s-%ld.log", log_dir,
               timestamp, (long)getpid());

  if (n < 0 || (size_t)n >= sizeof(filepath))
    return NULL;

  return filepath;
}
