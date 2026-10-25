/*
** Regression harness for reply encoding (src/mydns/reply.c).
** HARNESS-REPLACES: reply.o
**
** TXT: stored data is NUL-separated strings; each must go on the wire as one or more
** <character-string>s of at most 255 octets, with an RDLENGTH that matches what is written.
*/
#include "config.h"
#include "reply.c"				/* the real src/mydns/reply.c */
#include "common.h"

/*
** Encode a TXT record holding `data' (len bytes) for owner "a." and check the RDATA is a
** well-formed sequence of <character-string>s whose lengths are `expect' (terminated by -1)
** and whose concatenated contents equal `data' with the separators removed.
*/
static int
txt_ok(const char *data, size_t len, const int *expect) {
  TASK		*t = ALLOCATE(sizeof(TASK), TASK);
  RR		r;
  MYDNS_RR	rr;
  unsigned char	*p, *end;
  size_t	rdlength, n, i = 0;
  char		joined[4096];
  size_t	jlen = 0, k;
  int		ok = 1;

  memset(&r, 0, sizeof(r));
  memset(&rr, 0, sizeof(rr));
  strcpy((char *)r.name, "a.");
  r.rr = &rr;
  rr.type = DNS_QTYPE_TXT;
  rr.ttl = 60;
  MYDNS_RR_DATA_VALUE(&rr) = exact_copy(data, len);
  MYDNS_RR_DATA_LENGTH(&rr) = len;

  if (reply_add_txt(t, &r) < 0)
    return 0;

  /* owner "a." = 01 'a' 00, then TYPE CLASS TTL = 8 octets, then RDLENGTH */
  p = (unsigned char *)t->rdata + 3 + 8;
  rdlength = (p[0] << 8) | p[1];
  p += 2;
  end = p + rdlength;
  if ((size_t)((char *)end - t->rdata) != t->rdlen)
    ok = 0;						/* RDLENGTH disagrees with bytes written */

  while (ok && p < end) {
    n = *p++;
    if (expect[i] < 0 || (int)n != expect[i] || p + n > end)
      ok = 0;
    else {
      memcpy(joined + jlen, p, n);
      jlen += n;
      p += n;
      i++;
    }
  }
  if (expect[i] != -1)
    ok = 0;						/* Fewer strings than expected */

  for (k = 0, n = 0; ok && k < len; k++)		/* Data minus separators */
    if (data[k] != '\0' && (n >= jlen || joined[n++] != data[k]))
      ok = 0;

  free(MYDNS_RR_DATA_VALUE(&rr));
  RELEASE(t->rdata);
  RELEASE(t);
  return ok;
}

int
main(void) {
  static char buf[1024];

  { static const int e[] = { 5, -1 };
    check(txt_ok("hello", 5, e), "R1", "short TXT is one <character-string>"); }

  { static const int e[] = { 255, 45, -1 };
    memset(buf, 'x', 300);
    check(txt_ok(buf, 300, e), "R2", "300-octet string split into 255 + 45 (was malformed)"); }

  { static const int e[] = { 200, 200, -1 };
    memset(buf, 'a', 200); buf[200] = '\0'; memset(buf + 201, 'b', 200);
    check(txt_ok(buf, 401, e), "R3", "two stored strings become two <character-string>s"); }

  { static const int e[] = { 0, -1 };
    check(txt_ok("", 0, e), "R4", "empty TXT is a single empty <character-string>"); }

  { static const int e[] = { 255, -1 };
    memset(buf, 'y', 255);
    check(txt_ok(buf, 255, e), "R5", "exactly 255 octets is one <character-string>"); }

  { static const int e[] = { 255, 255, 3, -1 };
    memset(buf, 'z', 510); buf[510] = '\0'; memcpy(buf + 511, "end", 3);
    check(txt_ok(buf, 514, e), "R6", "510-octet string + short string -> 255, 255, 3"); }

  return failures();
}
