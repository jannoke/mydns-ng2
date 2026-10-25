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

  { /* text_retrieve(): three strings -> NUL-joined, length includes the separators.
       The old code under-allocated by one byte per separator (heap overflow). */
    static const unsigned char p[] = { 2, 'a', 'b', 1, 'c', 3, 'd', 'e', 'f' };
    uchar *buf = exact_copy(p, sizeof(p)), *src = buf;
    size_t len = 0;
    char *d = text_retrieve(&src, buf + sizeof(p), &len, 0);
    check(d && len == 8 && !memcmp(d, "ab\0c\0def", 9) && src == buf + sizeof(p),
	  "X1", "multi-string TXT data retrieved in bounds");
    RELEASE(d);
    free(buf);
  }
  { /* A string claiming more bytes than remain is truncated, not over-read. */
    static const unsigned char p[] = { 9, 'x', 'y' };
    uchar *buf = exact_copy(p, sizeof(p)), *src = buf;
    size_t len = 0;
    char *d = text_retrieve(&src, buf + sizeof(p), &len, 0);
    check(d && len == 2 && !strcmp(d, "xy"), "X2", "truncated string stays in bounds");
    RELEASE(d);
    free(buf);
  }
  { /* No data left (e.g. HINFO's second string missing): an empty string, never NULL.
       The old code wrote through a NULL pointer here. */
    static const unsigned char p[] = { 0 };
    uchar *buf = exact_copy(p, sizeof(p)), *src = buf + 1;
    size_t len = 99;
    char *d = text_retrieve(&src, buf + sizeof(p), &len, 1);
    check(d && len == 0 && d[0] == '\0', "X3", "no data yields an empty string, not NULL");
    RELEASE(d);
    free(buf);
  }
  { /* one_word_only stops after the first string (HINFO CPU / OS). */
    static const unsigned char p[] = { 3, 'c', 'p', 'u', 2, 'o', 's' };
    uchar *buf = exact_copy(p, sizeof(p)), *src = buf;
    size_t l1 = 0, l2 = 0;
    char *d1 = text_retrieve(&src, buf + sizeof(p), &l1, 1);
    char *d2 = text_retrieve(&src, buf + sizeof(p), &l2, 1);
    check(!strcmp(d1, "cpu") && !strcmp(d2, "os") && l1 == 3 && l2 == 2,
	  "X4", "HINFO-style one-word retrieval");
    RELEASE(d1);
    RELEASE(d2);
    free(buf);
  }

  return failures();
}
