/**************************************************************************************************
	$Id: sql.c,v 1.22 2005/04/20 16:40:25 bboy Exp $

	Copyright (C) 2002-2005  Don Moore <bboy@bboy.net>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at Your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
**************************************************************************************************/

#include "mydns.h"

SQL *sql;						/* Global SQL connection information */

/* Saved connection information for reconnecting */
static char *_sql_user = NULL;
static char *_sql_password = NULL;
static char *_sql_host = NULL;
static char *_sql_database = NULL;

/* db-timeout and db-retry-interval, set by load_config() */
int sql_timeout = 3;					/* Connect/read/write timeout, seconds (0 = none) */
int sql_retry_interval = 10;				/* Seconds to fail at once after losing the server */

/* State of the connection to the server, see sql_unavailable() */
static time_t sql_down_since = 0;			/* When it was lost (sql_now(); 0 = it's fine) */
static time_t sql_retry_at = 0;				/* Don't talk to the server before then */
#if USE_PGSQL
static int sql_pg_broken = 0;				/* Failed mid-query: needs a new connection */
static int sql_pg_timed_out = 0;			/* ... because the server didn't answer */
#endif


/* A clock that setting the date can't move: the retry window and timeouts use it. */
static long long
sql_now_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#define sql_now() ((time_t)(sql_now_ms() / 1000))


/**************************************************************************************************
	SQL_UNAVAILABLE
	After a connection error (the server went away, refused, or didn't answer within
	db-timeout), queries fail at once for db-retry-interval seconds instead of each one
	waiting for the timeout again: a server that hangs would otherwise stall every lookup,
	cached answers included, since the server handles one query at a time. Returns 1 while
	that is the case, so callers can skip their own per-query error messages.
**************************************************************************************************/
int
sql_unavailable(void) {
  return (sql_down_since && sql_now() < sql_retry_at);
}

static void
sql_mark_down(const char *why) {
  time_t now = sql_now();

  sql_retry_at = now + (sql_retry_interval > 0 ? sql_retry_interval : 0);
  if (!sql_down_since) {
    sql_down_since = now ? now : 1;		/* 0 means "up" */
    Warnx(_("database connection failed (%s); queries fail for %d s before reconnecting"),
	  why, sql_retry_interval);
  } else {
    Warnx(_("database still unavailable after %ld s (%s)"), (long)(now - sql_down_since), why);
  }
}

/* The server answered (with rows or an SQL error): the connection works. */
static void
sql_mark_up(void) {
  if (sql_down_since) {
    Notice(_("database connection restored after %ld s"), (long)(sql_now() - sql_down_since));
    sql_down_since = 0;
    sql_retry_at = 0;
  }
}
/*--- sql_unavailable() -------------------------------------------------------------------------*/


#if USE_PGSQL
/**************************************************************************************************
	SQL_PG_CONNECT
	PQsetdbLogin() takes no connect timeout, so connect with PQconnectdbParams(). NULL values
	are left out, which means libpq's defaults (e.g. the UNIX socket for no host).
**************************************************************************************************/
static PGconn *
sql_pg_connect(const char *host, const char *port, const char *database,
	       const char *user, const char *password) {
  char timeout[16];
  const char *keys[] = { "host", "port", "dbname", "user", "password", "connect_timeout", NULL };
  const char *values[7];

  snprintf(timeout, sizeof(timeout), "%d", sql_timeout);
  values[0] = host;
  values[1] = port;
  values[2] = database;
  values[3] = user;
  values[4] = password;
  values[5] = sql_timeout > 0 ? timeout : NULL;
  values[6] = NULL;
  return PQconnectdbParams(keys, values, 0);
}


/**************************************************************************************************
	SQL_PG_EXEC
	PQexec() with db-timeout: libpq has no read timeout, so send the query and wait for the
	answer with poll(). Returns the last result like PQexec(), or NULL. After a timeout the
	connection still has a query in flight and can't be used again: it's marked broken and
	replaced on the next attempt.
**************************************************************************************************/
static PGresult *
sql_pg_exec(PGconn *conn, const char *query) {
  PGresult *res = NULL, *last = NULL;
  long long deadline;

  if (sql_pg_broken)
    return (NULL);
  sql_pg_timed_out = 0;
  if (sql_timeout <= 0)
    return (PQexec(conn, query));

  deadline = sql_now_ms() + (long long)sql_timeout * 1000;
  if (!PQsendQuery(conn, query))
    return (NULL);
  for (;;) {
    while (PQisBusy(conn)) {
      struct pollfd pfd;
      long long left = deadline - sql_now_ms();
      int rv = 1;

      pfd.fd = PQsocket(conn);
      pfd.events = POLLIN;
      pfd.revents = 0;
      if (left <= 0 || pfd.fd < 0
	  || ((rv = poll(&pfd, 1, (int)left)) < 0 && errno != EINTR) || rv == 0
	  || !PQconsumeInput(conn)) {
	sql_pg_timed_out = (left <= 0 || rv == 0);
	sql_pg_broken = 1;
	if (last)
	  PQclear(last);
	return (NULL);
      }
    }
    if (!(res = PQgetResult(conn)))
      break;
    if (last)
      PQclear(last);
    last = res;
  }
  return (last);
}

/* After a failed query: was it the connection, rather than the SQL? */
static int
sql_pg_connection_error(PGconn *conn) {
  return (sql_pg_broken || !conn || PQstatus(conn) == CONNECTION_BAD);
}

static void
sql_pg_failed(PGconn *conn, PGresult *result) {
  if (sql_pg_connection_error(conn)) {
    char *msg = STRDUP(sql_pg_timed_out ? _("no answer within db-timeout") : PQerrorMessage(conn)), *c;

    for (c = msg; *c; c++)
      if (*c == '\r' || *c == '\n')
	*c = ' ';
    strtrim(msg);
    sql_mark_down(msg);
    RELEASE(msg);
  } else {
    sql_mark_up();
    WarnSQL(conn, "%s", _("error during query"));	/* WarnSQL adds the server's message */
  }
}
#else
/**************************************************************************************************
	SQL_MYSQL_OPTIONS
	Options for every connection: the [client] group of my.cnf, automatic reconnects, and
	db-timeout for connecting (the handshake included), reading and writing.
**************************************************************************************************/
static void
sql_mysql_options(MYSQL *conn) {
  unsigned int timeout = sql_timeout > 0 ? (unsigned int)sql_timeout : 0;
  char reconnect = 1;					/* my_bool / bool, one byte either way */

#if MYSQL_VERSION_ID > 32349
  mysql_options(conn, MYSQL_READ_DEFAULT_GROUP, "client");
#endif
#if MYSQL_VERSION_ID > 50012
  mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);
#endif
  if (timeout) {
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
#if MYSQL_VERSION_ID > 40100
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
#endif
  }
}

/* After a failed call: was it the connection, rather than the SQL? */
static int
sql_mysql_connection_error(MYSQL *conn) {
  switch (mysql_errno(conn)) {
  case 2002:						/* CR_CONNECTION_ERROR */
  case 2003:						/* CR_CONN_HOST_ERROR */
  case 2005:						/* CR_UNKNOWN_HOST */
  case 2006:						/* CR_SERVER_GONE_ERROR */
  case 2013:						/* CR_SERVER_LOST (timeouts too) */
  case 2055:						/* CR_SERVER_LOST_EXTENDED */
  case 1927:						/* ER_CONNECTION_KILLED (MariaDB) */
    return (1);
  }
  return (0);
}

static void
sql_mysql_failed(MYSQL *conn) {
  if (sql_mysql_connection_error(conn)) {
    sql_mark_down(mysql_error(conn));
  } else {
    sql_mark_up();
    if (mysql_error(conn)[0] != '\0')
      WarnSQL(conn, _("%s: error during query"), mysql_error(conn));
  }
}
#endif


/**************************************************************************************************
	SQL_SPLIT_HOST
	Splits "host" or "host:port" into a copy of the host name and the port, as a number for
	MySQL (0 = the default) and as a string for libpq (NULL = the default). `host' itself is
	never modified: it's the saved copy that every reconnect uses. Free *name and *portstr
	with sql_free_host().
**************************************************************************************************/
static void
sql_split_host(const char *host, char **name, unsigned int *port, char **portstr) {
  const char *colon = host ? strchr(host, ':') : NULL;

  *name = NULL;
  *port = 0;
  *portstr = NULL;
  if (!host)
    return;
  if (colon) {
    *name = STRNDUP(host, colon - host);
    *portstr = STRDUP(colon + 1);
    *port = atoi(colon + 1);
  } else {
    *name = STRDUP(host);
  }
}

static void
sql_free_host(char **name, char **portstr) {
  if (*name) RELEASE(*name);
  if (*portstr) RELEASE(*portstr);
}
/*--- sql_split_host() --------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_OPEN
	Connect to the database.  Errors fatal.
**************************************************************************************************/
void
sql_open(const char *user, const char *password, const char *host, const char *database) {
  char *hostname = NULL, *portstr = NULL;
  unsigned int port = 0;

  /* Save connection information so that we can reconnect if necessary */
  if (_sql_user) RELEASE(_sql_user);
  if (_sql_password) RELEASE(_sql_password);
  if (_sql_host) RELEASE(_sql_host);
  if (_sql_database) RELEASE(_sql_database);
  _sql_user = user ? STRDUP(user) : NULL;
  _sql_password = password ? STRDUP(password) : NULL;
  _sql_host = host ? STRDUP(host) : NULL;
  _sql_database = database ? STRDUP(database) : NULL;

  sql_split_host(host, &hostname, &port, &portstr);

#if USE_PGSQL
  sql = sql_pg_connect(hostname, portstr, database, user, password);
  if (PQstatus(sql) == CONNECTION_BAD) {
    char *errmsg = PQerrorMessage(sql), *c, *out = NULL;

    /* Save the first error message so that the user gets the error message they "expect" */
    for (c = errmsg; *c; c++)
      if (*c == '\r' || *c == '\n')
	*c = ' ';
    strtrim(errmsg);

    ASPRINTF(&out, "%s %s: %s (errno=%d)",
	     _("Error connecting to PostgreSQL server at"), host, errmsg, errno);

    if (sql)
      PQfinish(sql);
    /* Try login via UNIX socket before failing, per Lee Brotherston <lee@nerds.org.uk> */
    sql = sql_pg_connect(NULL, NULL, database, user, password);
    if (PQstatus(sql) == CONNECTION_BAD)
      Errx("%s", out);
    RELEASE(out);
  }
#else
  sql = ALLOCATE(sizeof(*sql), MYSQL);
  if (!mysql_init(sql)) {
    Err(_("Unable to allocate MySQL data structure"));
  }
  sql_mysql_options(sql);
  if (!(mysql_real_connect(sql, hostname, user, password, database, port, NULL, 0)))
    ErrSQL(sql, _("Error connecting to MySQL server at %s"), host);
#endif

  sql_free_host(&hostname, &portstr);

  /* A new connection (e.g. in a process forked while the old one was down) starts clean. */
  sql_down_since = sql_retry_at = 0;
#if USE_PGSQL
  sql_pg_broken = sql_pg_timed_out = 0;
#endif
}
/*--- sql_open() --------------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_REOPEN
	Attempt to close and reopen the database connection.
**************************************************************************************************/
void
sql_reopen(void) {
  SQL *new_sql = NULL;
  char *hostname = NULL, *portstr = NULL;
  unsigned int port = 0;

  if (sql_unavailable())				/* Lost it just now: don't wait for it again */
    return;

#if USE_PGSQL
  if (sql && !sql_pg_connection_error(sql))
    return;
#else
  if (!mysql_ping(sql)) {
    sql_mark_up();
    return;
  }
#endif

  sql_split_host(_sql_host, &hostname, &port, &portstr);

#if USE_PGSQL
  new_sql = sql_pg_connect(hostname, portstr, _sql_database, _sql_user, _sql_password);
  if (PQstatus(new_sql) == CONNECTION_BAD) {
    char *why = STRDUP(PQerrorMessage(new_sql)), *c;

    for (c = why; *c; c++)
      if (*c == '\r' || *c == '\n')
	*c = ' ';
    strtrim(why);
    PQfinish(new_sql);
    /* Try login via UNIX socket before failing, per Lee Brotherston <lee@nerds.org.uk> */
    new_sql = sql_pg_connect(NULL, NULL, _sql_database, _sql_user, _sql_password);
    if (PQstatus(new_sql) == CONNECTION_BAD) {
      PQfinish(new_sql);
      sql_mark_down(why);
      RELEASE(why);
      sql_free_host(&hostname, &portstr);
      return;
    }
    RELEASE(why);
  }
  sql_pg_broken = 0;
#else
  new_sql = ALLOCATE(sizeof(*new_sql), MYSQL);
  if (!mysql_init(new_sql)) {
    RELEASE(new_sql);
    sql_free_host(&hostname, &portstr);
    return;
  }
  sql_mysql_options(new_sql);
  if (!(mysql_real_connect(new_sql, hostname, _sql_user, _sql_password, _sql_database, port, NULL, 0))) {
    sql_mark_down(mysql_error(new_sql));
    mysql_close(new_sql);
    RELEASE(new_sql);
    sql_free_host(&hostname, &portstr);
    return;
  }
#endif

  sql_free_host(&hostname, &portstr);
  sql_close(sql);
  sql = new_sql;
  sql_mark_up();
}
/*--- sql_reopen() ------------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_ISTABLE
	Returns 1 if the specified table exists in the current database, or 0 if it does not.
**************************************************************************************************/
int
sql_istable(SQL *sqlConn, const char *tablename) {
  char *xtablename;
#if !USE_PGSQL
  SQL_RES *res;
#endif
  int rv = 0;

  xtablename = sql_escstr(sqlConn, (char*)tablename);

#if USE_PGSQL
  if (sql_count(sqlConn, "SELECT COUNT(*) FROM pg_class"
		" WHERE (relkind='r' OR relkind='v') AND relname='%s'", xtablename) > 0)
    rv = 1;
#else
  if ((res = sql_queryf(sqlConn, "SHOW TABLES LIKE '%s'", xtablename))) {
    if (sql_num_rows(res) > 0)
      rv = 1;
    sql_free(res);
  }
#endif

  RELEASE(xtablename);
  return (rv);
}
/*--- sql_istable() -----------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_ISCOLUMN
	Returns 1 if the specified column exists in the current database, or 0 if it does not.
**************************************************************************************************/
int
sql_iscolumn(SQL *sqlConn, const char *tablename, const char *columnname) {
  char *xtablename, *xcolumnname;
#if !USE_PGSQL
  SQL_RES *res;
#endif
  int rv = 0;

  xtablename = sql_escstr(sqlConn, (char*)tablename);
  xcolumnname = sql_escstr(sqlConn, (char*)columnname);

#if USE_PGSQL
  if (sql_count(sqlConn,
		"SELECT COUNT(*)"
		" FROM pg_class,pg_attribute"
		" WHERE (relkind='r' OR relkind='v')"
		" AND relname='%s'"
		" AND attrelid=oid"
		" AND attname='%s'", xtablename, xcolumnname) > 0)
    rv = 1;
#else
  if ((res = sql_queryf(sqlConn, "SHOW COLUMNS FROM %s LIKE '%s'", xtablename, xcolumnname))) {
    if (sql_num_rows(res) > 0)
      rv = 1;
    sql_free(res);
  }
#endif

  RELEASE(xtablename);
  RELEASE(xcolumnname);
  return (rv);
}
/*--- sql_iscolumn() ----------------------------------------------------------------------------*/

int sql_get_column_width(SQL *sqlConn, const char *tablename, const char *columnname) {
  char *xtablename, *xcolumnname;
  int width = 0;

  xtablename = sql_escstr(sqlConn, (char*)tablename);
  xcolumnname = sql_escstr(sqlConn, (char*)columnname);

  {
    SQL_RES *res = sql_queryf(sqlConn,
			      "SELECT character_maximum_length FROM information_schema.columns "
			      "WHERE table_name='%s' AND column_name='%s'",
			      xtablename, xcolumnname);
    SQL_ROW row;

    row = sql_getrow(res, NULL);
    if (row) {
      width = atoi(row[0]);
    }
    sql_free(res);
  }

  RELEASE(xtablename);
  RELEASE(xcolumnname);
  return (width);
}
/**************************************************************************************************
	_SQL_CLOSE
**************************************************************************************************/
void
_sql_close(SQL *sqlConn) {
#if USE_PGSQL
  PQfinish(sqlConn);
#else
  mysql_close(sqlConn);
  RELEASE(sqlConn);
#endif
}
/*--- _sql_close() ------------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_READY
	Called before every query. Returns 0 if the query should fail at once (see
	sql_unavailable()). A PostgreSQL connection that broke is replaced first, since libpq
	doesn't reconnect by itself (MySQL's does, MYSQL_OPT_RECONNECT).
**************************************************************************************************/
static int
sql_ready(SQL **sqlConn) {
  if (sql_unavailable())
    return (0);
#if USE_PGSQL
  if (*sqlConn == sql && sql_pg_connection_error(sql)) {
    sql_reopen();
    if (sql_pg_connection_error(sql))
      return (0);
    *sqlConn = sql;
  }
#endif
  return (1);
}
/*--- sql_ready() -------------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_NRQUERY
	Issues an SQL query that does not return a result.  Returns 0 on success, -1 on error.
**************************************************************************************************/
int
sql_nrquery(SQL *sqlConn, const char *query, size_t querylen) {
  if (!sql_ready(&sqlConn))
    return (-1);

#if USE_PGSQL
  {
    PGresult *result = sql_pg_exec(sqlConn, query);

    if (PQresultStatus(result) == PGRES_COMMAND_OK) {
      PQclear(result);
      sql_mark_up();
      return (0);
    }
    sql_pg_failed(sqlConn, result);
    PQclear(result);
    return (-1);
  }
#else
  if (mysql_real_query(sqlConn, query, querylen)) {
    sql_mysql_failed(sqlConn);
    return (-1);
  }
  sql_mark_up();
  return (0);
#endif
}
/*--- sql_nrquery() -----------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_QUERY
	Returns a query's result, or NULL on error.
**************************************************************************************************/
SQL_RES *
sql_query(SQL *sqlConn, const char *query, size_t querylen) {
  SQL_RES *res = NULL;

  if (!sql_ready(&sqlConn))
    return (NULL);

#if USE_PGSQL
  {
    PGresult *result = sql_pg_exec(sqlConn, query);
    ExecStatusType q_rv = PQresultStatus(result);

    if (q_rv == PGRES_TUPLES_OK) {
      res = ALLOCATE(sizeof(SQL_RES), SQL_RES);
      res->result = result;
      res->tuples = PQntuples(result);
      res->fields = PQnfields(result);
      res->current_tuple = 0;
      res->current_row = ALLOCATE(res->fields * sizeof(unsigned char *), unsigned char[]);
      res->current_length = ALLOCATE(res->fields * sizeof(int *), int[]);
      sql_mark_up();
    } else if (q_rv == PGRES_COMMAND_OK) {
      PQclear(result);
      sql_mark_up();
      return (NULL);
    } else {
      sql_pg_failed(sqlConn, result);
      PQclear(result);
      return (NULL);
    }
  }
#else
  if (mysql_real_query(sqlConn, query, querylen)
      || !(res = mysql_store_result(sqlConn))) {
    sql_mysql_failed(sqlConn);
    return (NULL);
  }
  sql_mark_up();
#endif

  return (res);
}
/*--- sql_query() -------------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_QUERYF
	Like sql_query, but accepts varargs format.
**************************************************************************************************/
SQL_RES *
sql_queryf(SQL *sqlConn, const char *fmt, ...) {
  va_list	ap;
  char		*buf = NULL;
  size_t	buflen;
  SQL_RES	*res;

  va_start(ap, fmt);
  buflen = VASPRINTF(&buf, fmt, ap);
  va_end(ap);

  res = sql_query(sqlConn, buf, buflen);
  RELEASE(buf);

  return (res);
}
/*--- sql_queryf() ------------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_COUNT
	Provided a statement like "SELECT COUNT(*)..." returns the count returned, or -1 if an error
	occurred.
**************************************************************************************************/
long
sql_count(SQL *sqlConn, const char *fmt, ...) {
  va_list	ap;
  char		*buf = NULL;
  size_t	buflen;
  SQL_RES	*res = NULL;
  long		rv = 0;

  va_start(ap, fmt);
  buflen = VASPRINTF(&buf, fmt, ap);
  va_end(ap);

  res = sql_query(sqlConn, buf, buflen);
  RELEASE(buf);

  if (!res)
    return (-1);
  if (sql_num_rows(res)) {
#if USE_PGSQL
    rv = atol(PQgetvalue(res->result, 0, 0));
#else
    MYSQL_ROW row;

    if ((row = mysql_fetch_row(res)))
      rv = atol(row[0]);
#endif
  }
  sql_free(res);
  return (rv);
}
/*--- sql_count() -------------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_NUM_ROWS
	Returns the number of rows in a result set.
**************************************************************************************************/
long
sql_num_rows(SQL_RES *res) {
#if USE_PGSQL
  return res->tuples;
#else
  return mysql_num_rows(res);
#endif
}
/*--- sql_num_rows() ----------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_GETROW
	Returns the next row from the result, or NULL if no more rows exist.
**************************************************************************************************/
SQL_ROW
sql_getrow(SQL_RES *res, unsigned long **lengths) {
#if USE_PGSQL
  register int n;

  if (res->current_tuple >= res->tuples)
    return (NULL);
  for (n = 0; n < res->fields; n++) {
    res->current_row[n] = PQgetvalue(res->result, res->current_tuple, n);
    res->current_length[n] = PQgetlength(res->result, res->current_tuple, n);
  }
  if (lengths) *lengths = res->current_length;
  res->current_tuple++;
  return (res->current_row);
#else
  SQL_ROW row = mysql_fetch_row(res);
  if (lengths) *lengths = mysql_fetch_lengths(res);
  return row;
#endif
}
/*--- sql_getrow() ------------------------------------------------------------------------------*/


/**************************************************************************************************
	SQL_ESCSTR
	Escapes a string to make it suitable for an SQL query.
**************************************************************************************************/
char *
sql_escstr2(SQL *sqlConn, char *src, size_t srclen) {
  char *dest = ALLOCATE(srclen*2+1, char[]);
  size_t reslen;
#if USE_PGSQL
  int errcode = 0;
  reslen =   PQescapeStringConn(sqlConn, dest, src, srclen, &errcode);
#else
  reslen = mysql_real_escape_string(sqlConn, dest, src, srclen);
#endif
  dest = REALLOCATE(dest, reslen+1, char[]);
  return dest;
}

char *
sql_escstr(SQL *sqlConn, char *src) {
  return sql_escstr2(sqlConn, src, strlen(src));
}
/*--- sql_escstr() ------------------------------------------------------------------------------*/


/**************************************************************************************************
	_SQL_FREE
	Free an SQL result.
**************************************************************************************************/
void
_sql_free(SQL_RES *res) {
#if USE_PGSQL
  RELEASE(res->current_length);
  RELEASE(res->current_row);
  PQclear(res->result);
  RELEASE(res);
#else
  mysql_free_result(res);
#endif
}
/*--- _sql_free() -------------------------------------------------------------------------------*/
int
sql_build_query(char **query, const char *fmt, ...) {
  va_list ap;
  int querylen;

  va_start(ap, fmt);
  querylen = VASPRINTF(query, fmt, ap);
  va_end(ap);

  return querylen;
}

/* vi:set ts=3: */
