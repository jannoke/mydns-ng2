/*
** Regression harness for DNS UPDATE parsing (src/mydns/update.c).
** HARNESS-REPLACES: update.o
**
** Packets are placed in heap blocks of exactly their own size, so any read past the end is
** an ASAN error. Only the parsing helpers are called; no database is involved.
*/
#include "config.h"
#include "update.c"				/* the real src/mydns/update.c */
#include "common.h"

static TASK *
new_task(void) {
  TASK *t = ALLOCATE(sizeof(TASK), TASK);
  t->protocol = SOCK_DGRAM;
  t->family = AF_INET;
  return t;
}

/* Run update_gobble_rr() over `pkt', starting at offset `at'; returns the new position or NULL. */
static uchar *
gobble(const unsigned char *pkt, size_t len, size_t at, UQRR *rr, uchar **base) {
  TASK *t = new_task();
  uchar *q = exact_copy(pkt, len);
  uchar *r;

  memset(rr, 0, sizeof(*rr));
  r = update_gobble_rr(t, q, len, q + at, rr);
  *base = q;
  RELEASE(t);
  return r;
}

int
main(void) {
  UQRR rr;
  uchar *base, *r;

  { /* Name, then the packet ends: TYPE/CLASS/TTL/RDLENGTH must not be read. */
    static const unsigned char p[] = { 1, 'a', 0 };
    r = gobble(p, sizeof(p), 0, &rr, &base);
    check(r == NULL, "P1", "RR truncated after its name is rejected");
    free(base);
  }
  { /* RDLENGTH claims 1000 octets but only 4 follow: must not copy past the packet. */
    static const unsigned char p[] = { 1, 'a', 0,  0, 1,  0, 1,  0, 0, 0, 60,  0x03, 0xE8,
				       192, 0, 2, 1 };
    r = gobble(p, sizeof(p), 0, &rr, &base);
    check(r == NULL, "P2", "RDLENGTH larger than the remaining packet is rejected");
    free(base);
  }
  { /* A well-formed A record still parses, and parsing continues right after it. */
    static const unsigned char p[] = { 1, 'a', 0,  0, 1,  0, 1,  0, 0, 0, 60,  0, 4,
				       192, 0, 2, 1 };
    r = gobble(p, sizeof(p), 0, &rr, &base);
    check(r == base + sizeof(p) && rr.type == DNS_QTYPE_A && UQRR_DATA_LENGTH(&rr) == 4
	  && !memcmp(UQRR_DATA_VALUE(&rr), "\xC0\x00\x02\x01", 4),
	  "P3", "well-formed A record parses");
    free(base);
  }

  return failures();
}
