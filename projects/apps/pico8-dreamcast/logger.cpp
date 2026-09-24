#include "logger.h"
#include <cstdarg>
#include <cstdio>

void Logger_Initialize(const char*) {}
void Logger_Exit() {}
void Logger_Write(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}
void Logger_WriteUnformatted(const char* message) { printf("%s", message); }
void Logger_LogOutput(const char* func, size_t line, const char* format, ...) {
    printf("%s:%u: ", func, unsigned(line));
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}
