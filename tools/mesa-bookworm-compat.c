#include <stdarg.h>
#include <stdio.h>

extern int __isoc99_vsscanf(const char *str, const char *format, va_list args);
int __isoc23_sscanf(const char *str, const char *format, ...);

int __isoc23_sscanf(const char *str, const char *format, ...)
{
    int result;
    va_list args;

    va_start(args, format);
    result = __isoc99_vsscanf(str, format, args);
    va_end(args);
    return result;
}