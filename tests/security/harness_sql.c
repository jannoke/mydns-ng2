/*
** Regression harness for the database connection code (src/lib/sql.c).
**
** sql_reopen() cut the saved "host:port" at the colon to get the port and never put the
** colon back when the server answered the ping or the reconnect failed, so from then on
** every reconnect went to the default port. For PostgreSQL it passed ":5433", colon
** included, as the port. Every failed MySQL reconnect also leaked its MYSQL struct.
**
** The reconnects here go to 127.0.0.1 port 1, which refuses at once: no DNS traffic and
** no real database are involved.
*/
#include "config.h"
#include "sql.c"				/* the real src/lib/sql.c, statics included */
#include "common.h"

static void
split_case(const char *in, const char *want_name, unsigned int want_port, const char *want_portstr,
	   const char *id, const char *what) {
  char *copy = in ? STRDUP(in) : NULL, *name = NULL, *portstr = NULL;
  unsigned int port = 12345;
  int ok;

  sql_split_host(copy, &name, &port, &portstr);
  ok = port == want_port
    && ((!name && !want_name) || (name && want_name && !strcmp(name, want_name)))
    && ((!portstr && !want_portstr) || (portstr && want_portstr && !strcmp(portstr, want_portstr)))
    && ((!copy && !in) || (copy && in && !strcmp(copy, in)));
  check(ok, id, what);
  sql_free_host(&name, &portstr);
  if (copy) RELEASE(copy);
}

/* An unconnected handle to a port that refuses, like the global one after the server went away. */
static SQL *
refused_handle(void) {
#if USE_PGSQL
  return PQsetdbLogin("127.0.0.1", "1", NULL, NULL, "mydns", "mydns", "x");
#else
  SQL *h = ALLOCATE(sizeof(*h), MYSQL);

  mysql_init(h);
  mysql_real_connect(h, "127.0.0.1", "mydns", "x", "mydns", 1, NULL, 0);
  return h;
#endif
}

int
main(void) {
  split_case("db.example:3307", "db.example", 3307, "3307", "S1", "host:port splits into host and port");
  split_case("db.example", "db.example", 0, NULL, "S2", "host alone means the default port");
  split_case(NULL, NULL, 0, NULL, "S3", "no host at all");

  { /* sql_reopen() against a refusing server must leave the saved host:port as it was. */
    int n;

    _sql_host = STRDUP("127.0.0.1:1");
    _sql_user = STRDUP("mydns");
    _sql_password = STRDUP("x");
    _sql_database = STRDUP("mydns");
    sql = refused_handle();

    sql_reopen();
    check(!strcmp(_sql_host, "127.0.0.1:1"), "S4", "a failed reconnect keeps the port in the saved host");
    for (n = 0; n < 3; n++)
      sql_reopen();
    check(!strcmp(_sql_host, "127.0.0.1:1"), "S5", "so do repeated ones");
    check(sql != NULL, "S6", "the old handle stays in place when reconnecting fails");
  }

  return failures();
}
