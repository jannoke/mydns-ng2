/*
** Regression harness for the database connection code (src/lib/sql.c).
**
** sql_reopen() cut the saved "host:port" at the colon to get the port and never put the
** colon back when the server answered the ping or the reconnect failed, so from then on
** every reconnect went to the default port. For PostgreSQL it passed ":5433", colon
** included, as the port. Every failed MySQL reconnect also leaked its MYSQL struct.
**
** After a connection error, queries fail at once for db-retry-interval seconds instead of
** each one waiting for db-timeout (W1-W5): a hanging database used to stall the whole server.
**
** The reconnects here go to 127.0.0.1 port 1, which refuses at once: no DNS traffic and
** no real database are involved.
*/
#include "config.h"
#include "sql.c"				/* the real src/lib/sql.c, statics included */
#include "common.h"
#include <sys/time.h>

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
    for (n = 0; n < 3; n++) {
      sql_down_since = 0;				/* past the retry window, so it really reconnects */
      sql_reopen();
    }
    check(!strcmp(_sql_host, "127.0.0.1:1"), "S5", "so do repeated ones");
    check(sql != NULL, "S6", "the old handle stays in place when reconnecting fails");
  }

  { /* The retry window after a connection error. */
    struct timeval a, b;
    SQL_RES *res;
    int rv;

    sql_retry_interval = 10;
    sql_down_since = sql_retry_at = 0;
    sql_reopen();					/* refused: the server is gone */
    check(sql_unavailable(), "W1", "a refused reconnect opens the retry window");

#if !USE_PGSQL
    {
      unsigned int before = mysql_errno(sql);

      gettimeofday(&a, NULL);
      res = sql_query(sql, "SELECT 1", 8);
      rv = sql_nrquery(sql, "DO 1", 4);
      gettimeofday(&b, NULL);
      check(!res && rv == -1 && mysql_errno(sql) == before, "W2",
	    "inside it, queries fail without calling the client library");
    }
#else
    gettimeofday(&a, NULL);
    res = sql_query(sql, "SELECT 1", 8);
    rv = sql_nrquery(sql, "SELECT 1", 8);
    gettimeofday(&b, NULL);
    check(!res && rv == -1, "W2", "inside it, queries fail at once");
#endif
    check((b.tv_sec - a.tv_sec) * 1000000 + (b.tv_usec - a.tv_usec) < 100000, "W3",
	  "and take no time");

    sql_retry_at = sql_now() - 1;			/* the window is over */
    check(!sql_unavailable(), "W4", "after db-retry-interval the server is tried again");
    res = sql_query(sql, "SELECT 1", 8);
    check(!res && sql_unavailable(), "W5", "a failed retry opens the next window");
  }

  return failures();
}
