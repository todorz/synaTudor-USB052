#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "internal.h"

typedef struct {
    DWORD dwLowDateTime, dwHighDateTime;
} FILETIME;

typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

__winfnc BOOL QueryPerformanceCounter(uint64_t *counter) {
    struct timespec time;
    if(clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        winerr_set_errno();
        return FALSE;
    }

    //Nanosecond ticks - must match QueryPerformanceFrequency below (this used to add tv_nsec to
    //tv_sec scaled to microseconds, which isn't a consistent unit at all)
    *counter = time.tv_sec * 1000000000lu + time.tv_nsec;
    return TRUE;
}
WINAPI(QueryPerformanceCounter)

__winfnc BOOL QueryPerformanceFrequency(uint64_t *freq) {
    *freq = 1000000000lu;
    return TRUE;
}
WINAPI(QueryPerformanceFrequency)

__winfnc DWORD GetTickCount() {
    //Milliseconds, like the real thing
    uint64_t counter;
    if(!QueryPerformanceCounter(&counter)) { log_error("QueryPerformanceCounter failed!"); abort(); }
    return (DWORD) (counter / 1000000lu);
}
WINAPI(GetTickCount)

__winfnc void GetSystemTimeAsFileTime(FILETIME *outTime) {
    struct timeval time;
    assert(gettimeofday(&time, NULL) == 0);

    //100ns intervals since 1601-01-01 (11644473600s before the Unix epoch)
    uint64_t ft = (time.tv_sec + 11644473600lu) * 10000000lu + time.tv_usec * 10lu;
    outTime->dwLowDateTime =  (DWORD) ((ft >>  0) & 0xffffffffu);
    outTime->dwHighDateTime = (DWORD) ((ft >> 32) & 0xffffffffu);
}
WINAPI(GetSystemTimeAsFileTime)

__winfnc void GetLocalTime(SYSTEMTIME *out_time) {
    struct timeval tv;
    assert(gettimeofday(&tv, NULL) == 0);

    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    out_time->wYear = (WORD) (tm.tm_year + 1900);
    out_time->wMonth = (WORD) (tm.tm_mon + 1);
    out_time->wDayOfWeek = (WORD) tm.tm_wday;
    out_time->wDay = (WORD) tm.tm_mday;
    out_time->wHour = (WORD) tm.tm_hour;
    out_time->wMinute = (WORD) tm.tm_min;
    out_time->wSecond = (WORD) tm.tm_sec;
    out_time->wMilliseconds = (WORD) (tv.tv_usec / 1000);
}
WINAPI(GetLocalTime)