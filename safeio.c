#include "safeio.h"
#include <stdio.h>

int snprintf(char* buffer, size_t bufsz, const char* format, ...) {
    va_list args;
    int result;

    va_start(args, format);
    result = vsprintf(buffer, format, args);
    va_end(args);

    /* 手动检查是否溢出 */
    if (result >= bufsz) {
        /* 如果溢出，确保字符串以null结尾 */
        buffer[bufsz - 1] = '\0';
        return -1; /* 返回错误 */
    }

    return result;
}

