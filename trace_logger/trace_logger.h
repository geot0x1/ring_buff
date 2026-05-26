#ifndef TRACE_LOGGER_H
#define TRACE_LOGGER_H

#include <stdio.h>

#ifndef TRACE_LOG_PRINT
#define TRACE_LOG_PRINT(...) printf(__VA_ARGS__)
#endif

#ifndef PRINTF
#define PRINTF(...) TRACE_LOG_PRINT(__VA_ARGS__)
#endif

#endif /* TRACE_LOGGER_H */
