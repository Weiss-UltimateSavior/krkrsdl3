#include <stdio.h>
#include <time.h>

#include "tjsCommHead.h"

void TVPConsoleLog(const tjs_char* format, ...)
{
    va_list args;
    va_start(args, format);
    printf(format, args);
    printf("\n");
    va_end(args);
}

tjs_uint32 TVPGetRoughTickCount32()
{
    return (tjs_uint32)time(NULL) * 1000;
}