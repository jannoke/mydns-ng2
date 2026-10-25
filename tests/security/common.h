/*
** Shared helpers for the security regression harnesses (see run.sh).
*/
#ifndef _MYDNS_SECURITY_COMMON_H
#define _MYDNS_SECURITY_COMMON_H

#include <stddef.h>

/* Record one check: prints PASS/FAIL with `what' and counts failures. */
extern void check(int ok, const char *id, const char *what);

/* Number of failed checks so far; harness main() returns this. */
extern int  failures(void);

/*
** Copy `len' bytes into a fresh heap block of exactly `len' bytes, so that any read
** past the end of the "packet" lands in an ASAN redzone instead of silently reading
** slack space in a bigger buffer.
*/
extern unsigned char *exact_copy(const void *data, size_t len);

#endif
