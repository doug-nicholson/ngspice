/**********
Copyright 1990 Regents of the University of California.  All rights reserved.
**********/

/*
 * Date and time utility functions
 */

#include "ngspice/ngspice.h"
#include <string.h>
#include "misc_time.h"

#ifdef USE_OMP
#include <omp.h>
#endif

/* Return the date. Return value is static data. */

char *
datestring(void)
{

#ifdef HAVE_LOCALTIME
    static char tbuf[45];
    struct tm *tp;
    char *ap;
    size_t i;

    time_t tloc;
    time(&tloc);
    tp = localtime(&tloc);
    ap = asctime(tp);
    (void) sprintf(tbuf, "%.20s", ap);
    (void) strcat(tbuf, ap + 19);
    i = strlen(tbuf);
    tbuf[i - 1] = '\0';
    return (tbuf);

#else

    return ("today");

#endif
}

/* 
 * How many seconds have elapsed in running time. 
 * This is the routine called in IFseconds 
 */

double
seconds(void)
{
#ifdef USE_OMP
    // Usage of OpenMP time function
    return omp_get_wtime();
#elif defined(HAVE_QUERYPERFORMANCECOUNTER)
    // Windows (MSC and mingw) specific implementation
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / frequency.QuadPart;
#elif defined(HAVE_CLOCK_GETTIME)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#elif defined(HAVE_GETTIMEOFDAY)
    // Usage of gettimeofday
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
#elif defined(HAVE_TIMES)
    // Usage of times
    struct tms t;
    clock_t ticks = times(&t);
    return (double)ticks / sysconf(_SC_CLK_TCK);
#elif defined(HAVE_GETRUSAGE)
    // Usage of getrusage
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6;
#elif defined(HAVE_FTIME)
    // Usage of ftime
    struct timeb tb;
    ftime(&tb);
    return tb.time + tb.millitm / 1000.0;
#else
    error_no_timer_function_available;
#endif
}

void perf_timer_start(PerfTimer *timer)
{
    timer->start = seconds();
}

void perf_timer_stop(PerfTimer *timer)
{
    timer->end = seconds();
}

void perf_timer_elapsed_sec_ms(const PerfTimer *timer, int *seconds, int *milliseconds)
{
    double elapsed = timer->end - timer->start;
    *seconds = (int)elapsed;
    *milliseconds = (int)((elapsed - *seconds) * 1000.0);
}
