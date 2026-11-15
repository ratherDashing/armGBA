/* What the 3DS's C library (newlib) is without, for the core's and libretro's sources: included in
 * every file the 3DS build compiles (the Makefile's -include). */
#ifndef ARMGBA_3DS_COMPAT_H
#define ARMGBA_3DS_COMPAT_H
#include <time.h>
/* struct tm as UTC to seconds since 1970 (the Mega Duck laptop's clock). */
time_t timegm(struct tm* tm);
#endif
