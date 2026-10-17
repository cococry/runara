
#ifndef RUNARA_LOG_H
#define RUNARA_LOG_H

#include <stdbool.h>
#include <stdio.h>

enum rn_log_level_t {
  RN_LL_TRACE = 0,
  RN_LL_INFO,
  RN_LL_WARN,
  RN_LL_ERR,
  RN_LL_FATAL,
  RN_LL_COUNT
};

struct rn_log_t {
  FILE *stream;
  bool quiet;
  bool verbose;
};

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

#endif
