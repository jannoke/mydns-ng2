/*
** Shared helpers for the security regression harnesses (see run.sh and common.h).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

static int nfail = 0;

void
check(int ok, const char *id, const char *what) {
  printf("%s %-4s %s\n", ok ? "PASS" : "FAIL", id, what);
  if (!ok)
    nfail++;
}

int
failures(void) {
  return nfail;
}

unsigned char *
exact_copy(const void *data, size_t len) {
  unsigned char *p = malloc(len ? len : 1);

  if (!p)
    abort();
  memcpy(p, data, len);
  return p;
}
