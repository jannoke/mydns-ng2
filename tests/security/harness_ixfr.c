/*
** Regression harness for IXFR request parsing (src/mydns/ixfr.c).
** HARNESS-REPLACES: ixfr.o
**
** The authority section of an IXFR request carries the client's SOA; its fixed fields used
** to be read without checking they were inside the packet.
*/
#include "config.h"
#include "ixfr.c"				/* the real src/mydns/ixfr.c */
#include "common.h"

static uchar *
gobble(const unsigned char *pkt, size_t len, IARR *rr, uchar **base) {
  TASK *t = ALLOCATE(sizeof(TASK), TASK);
  uchar *q = exact_copy(pkt, len);
  uchar *r;

  memset(rr, 0, sizeof(*rr));
  r = ixfr_gobble_authority_rr(t, q, len, q, rr);
  *base = q;
  RELEASE(t);
  return r;
}

int
main(void) {
  IARR rr;
  uchar *base, *r;
  /* owner a. SOA IN ttl rdlen | mname b. | rname c. | serial refresh retry expire minimum */
  static const unsigned char soa[] = {
    1,'a',0,  0,6, 0,1, 0,0,0,60, 0,26,  1,'b',0,  1,'c',0,
    0,0,0,7,  0,0,0,1, 0,0,0,2, 0,0,0,3, 0,0,0,4 };

  r = gobble(soa, 3, &rr, &base);				/* just the owner name */
  check(r == NULL, "I1", "authority RR truncated after its name is rejected");
  free(base);

  r = gobble(soa, sizeof(soa) - 6, &rr, &base);			/* SOA counters cut short */
  check(r == NULL, "I2", "SOA counters running past the packet are rejected");
  free(base);

  r = gobble(soa, sizeof(soa), &rr, &base);
  check(r == base + sizeof(soa) && rr.serial == 7 && rr.minimum == 4,
	"I3", "well-formed authority SOA parses");
  free(base);

  return failures();
}
