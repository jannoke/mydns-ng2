/*
** Regression harness for the DNS name encoder (src/mydns/encode.c).
** HARNESS-REPLACES: encode.o
**
** Names come from the database (record names and targets), which DataPanel lets hosting
** clients edit, so malformed names must be rejected rather than encoded into a broken reply.
*/
#include "config.h"
#include "encode.c"				/* the real src/mydns/encode.c */
#include "common.h"

static int
encode(TASK *t, const char *name, unsigned int offset, char **out) {
  *out = NULL;
  return name_encode2(t, out, name, offset, 1);
}

int
main(void) {
  TASK *t = ALLOCATE(sizeof(TASK), TASK);
  char *out;
  int len;

  len = encode(t, "www.example.", 12, &out);
  check(len == 13 && !memcmp(out, "\3www\7example", 13), "E1", "plain name encodes");
  RELEASE(out);

  len = encode(t, "mail.example.", 40, &out);			/* "example." remembered at 16 */
  check(len == 7 && !memcmp(out, "\4mail\xC0\x10", 7), "E2", "suffix compressed to earlier name");
  RELEASE(out);

  t->hdr.rcode = 0;
  len = encode(t, "a..b.", 60, &out);
  check(len < 0, "E3", "empty label in the middle (a..b.) rejected");
  RELEASE(out);

  t->hdr.rcode = 0;
  len = encode(t, ".a.", 60, &out);
  check(len < 0, "E4", "leading empty label (.a.) rejected");
  RELEASE(out);

  t->hdr.rcode = 0;
  len = encode(t, "", 60, &out);
  check(len < 0, "E5", "empty string rejected (no read before the buffer)");
  RELEASE(out);

  { /* A name written beyond offset 0x3FFF must not become a compression target. */
    TASK *t2 = ALLOCATE(sizeof(TASK), TASK);
    len = encode(t2, "far.example.", 0x5000, &out);
    RELEASE(out);
    len = encode(t2, "x.far.example.", 0x5100, &out);
    check(len == 15 && !memcmp(out, "\1x\3far\7example", 15), "E6", "names past offset 16383 are not used as pointer targets");
    RELEASE(out);
    name_forget(t2);
    RELEASE(t2);
  }

  name_forget(t);
  RELEASE(t);
  return failures();
}
