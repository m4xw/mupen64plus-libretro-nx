#include "../wasm_dynarec.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void append(char **buf, size_t *size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (*buf == NULL) {
        *size = (size_t)needed + 1;
        *buf = malloc(*size);
        (*buf)[0] = '\0';
    } else {
        size_t len = strlen(*buf);
        if (len + needed + 1 > *size) {
            *size = len + needed + 1;
            *buf = realloc(*buf, *size);
        }
    }

    va_start(args, fmt);
    vsprintf(*buf + strlen(*buf), fmt, args);
    va_end(args);
}
