/*
** Regression harness for the DNS name decoder (src/lib/unencode.c).
**
** Every "packet" is placed in a heap block of exactly its own size (exact_copy), so a read
** past the end is an ASAN error rather than a silent read of slack space. run.sh wraps the
** binary in a timeout, so a decoder loop shows up as a failure, not a hang that passes.
*/
#include "config.h"
#include "unencode.c"				/* the real src/lib/unencode.c */
#include "common.h"

/* Decode `pkt' (len bytes) starting at offset `at'; returns the name or NULL. */
static char *
decode(const unsigned char *pkt, size_t len, size_t at, task_error_t *err, size_t *consumed) {
  uchar *buf = exact_copy(pkt, len);
  uchar *cur = buf + at;
  char *name = NULL;

  *err = 0;
  name = (char *)name_unencode2(buf, len, &cur, err);
  if (consumed)
    *consumed = cur - buf;
  free(buf);
  return name;
}

int
main(void) {
  task_error_t err;
  size_t used;
  char *n;

  { /* A pointer must point strictly backward; one to itself or forward is rejected. */
    static const unsigned char p[] = { 0xC0, 0x00 };
    n = decode(p, sizeof(p), 0, &err, NULL);
    check(!n && err == ERR_Q_INVALID_COMPRESSION, "U1", "self-referencing pointer rejected");
    free(n);
  }
  { /* The classic two-pointer cycle (2 -> 0 -> 2) must terminate with an error. */
    static const unsigned char p[] = { 0xC0, 0x02, 0xC0, 0x00 };
    n = decode(p, sizeof(p), 2, &err, NULL);
    check(!n, "U2", "pointer cycle 2->0->2 rejected (no hang)");
    free(n);
  }
  { /* A label followed by a pointer back to it loops through backward jumps only;
       the name-length cap has to stop it. */
    static const unsigned char p[] = { 3, 'a', 'b', 'c', 0xC0, 0x00 };
    n = decode(p, sizeof(p), 0, &err, NULL);
    check(!n && err == ERR_Q_NAME_TOO_LONG, "U3", "label+backward-pointer loop stopped by length cap");
    free(n);
  }
  { /* A label claiming more bytes than the packet has must not be read past the end. */
    static const unsigned char p[] = { 5, 'a', 'b' };
    n = decode(p, sizeof(p), 0, &err, NULL);
    check(!n && err == ERR_Q_BUFFER_OVERFLOW, "U4", "label running past end of packet rejected");
    free(n);
  }
  { /* A name with no terminating zero octet must not be read past the end. */
    static const unsigned char p[] = { 1, 'a' };
    n = decode(p, sizeof(p), 0, &err, NULL);
    check(!n && err == ERR_Q_BUFFER_OVERFLOW, "U5", "unterminated name rejected");
    free(n);
  }
  { /* A pointer whose second octet is missing. */
    static const unsigned char p[] = { 1, 'a', 0, 0xC0 };
    n = decode(p, sizeof(p), 3, &err, NULL);
    check(!n && err == ERR_Q_BUFFER_OVERFLOW, "U6", "truncated compression pointer rejected");
    free(n);
  }
  { /* A legitimate backward pointer still decodes, and parsing resumes after it. */
    static const unsigned char p[] = { 1, 'a', 0, 3, 'w', 'w', 'w', 0xC0, 0x00 };
    n = decode(p, sizeof(p), 3, &err, &used);
    check(n && !strcmp(n, "www.a.") && used == sizeof(p), "U7", "valid compressed name decodes");
    free(n);
  }
  { /* The root name. */
    static const unsigned char p[] = { 0 };
    n = decode(p, sizeof(p), 0, &err, &used);
    check(n && !strcmp(n, ".") && used == 1, "U8", "root name decodes to '.'");
    free(n);
  }
  { /* Over-long name (5 x 63-octet labels = 321 octets) is rejected, not overflowed. */
    unsigned char p[5 * 64 + 1];
    int i;
    for (i = 0; i < 5; i++) {
      p[i * 64] = 63;
      memset(&p[i * 64 + 1], 'x', 63);
    }
    p[5 * 64] = 0;
    n = decode(p, sizeof(p), 0, &err, NULL);
    check(!n && err == ERR_Q_NAME_TOO_LONG, "U9", "name over 255 octets rejected");
    free(n);
  }
  { /* Exactly 255 octets on the wire (3 x 63 + 61 + 4 length octets + terminator) is allowed. */
    unsigned char p[255];
    int i, o = 0;
    for (i = 0; i < 4; i++) {
      int l = (i < 3) ? 63 : 61;
      p[o++] = l;
      memset(&p[o], 'y', l);
      o += l;
    }
    p[o++] = 0;
    n = decode(p, o, 0, &err, &used);
    check(n && strlen(n) == 254 && used == 255, "U10", "maximum-length (255 octet) name accepted");
    free(n);
  }
  { /* The fixed-buffer API reports a too-small destination instead of overflowing it. */
    static const unsigned char p[] = { 3, 'a', 'b', 'c', 0 };
    uchar *buf = exact_copy(p, sizeof(p));
    uchar small[4];
    uchar *r = name_unencode(buf, sizeof(p), buf, small, sizeof(small));
    check(!r && small[0] == ERR_Q_BUFFER_OVERFLOW, "U11", "name_unencode() respects destsize");
    free(buf);
  }

  return failures();
}
