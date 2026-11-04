#!/bin/bash
# Database outages while mydns runs, and a database that is not there when it starts: a server that hangs (SIGSTOP: connections are accepted,
# nothing answers, like a network partition or `docker pause`) and one that is stopped.
# The database listens on a non-default port, so reconnects must keep the port.
#
# Usage: DBTYPE=mysql|pgsql WORK=<built tree> db-outage.sh    (inside the dev container)
#   mysql: WORK=/work after build.sh;  pgsql: a tree configured --without-mysql --with-pgsql
#
# Expected, with db-timeout = 2 and db-retry-interval = 4:
#   - the first uncached query during a hang gets SERVFAIL after about db-timeout;
#   - after that, cached names answer at once and uncached ones get SERVFAIL at once;
#   - once the database is back, answers return within db-retry-interval;
#   - the log has one "connection failed" and one "restored" line per outage.
DBTYPE=${DBTYPE:-mysql}
WORK=${WORK:-/work}
TIMEOUT=2 RETRY=4
LOG=/t/db-outage-$DBTYPE.log
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
fail=0
cd "$WORK" || exit 2

case $DBTYPE in
mysql)
  PORT=3307
  db_start()  { (mysqld_safe --port=$PORT >/dev/null 2>&1 &); for i in $(seq 30); do mysqladmin ping >/dev/null 2>&1 && return; sleep 1; done; echo "mysqld didn't start"; exit 2; }
  db_stop()   { mysqladmin shutdown >/dev/null 2>&1; for i in $(seq 30); do pidof mariadbd >/dev/null || return; sleep 0.5; done; }
  db_pids()   { pidof mariadbd; }
  db_sql()    { mysql mydns; }
  db_setup()  {
    db_stop; db_start
    mysql -e "DROP DATABASE IF EXISTS mydns; CREATE DATABASE mydns;
      CREATE USER IF NOT EXISTS mydns@127.0.0.1 IDENTIFIED BY 'mydns'; GRANT ALL ON mydns.* TO mydns@127.0.0.1;"
  }
  ;;
pgsql)
  PORT=5433
  db_start()  { pg_ctlcluster 17 main start || exit 2; }
  db_stop()   { pg_ctlcluster 17 main stop -m fast >/dev/null 2>&1; }
  db_pids()   { pgrep -u postgres -d ' ' .; }
  db_sql()    { su postgres -c "psql -q -p $PORT -v ON_ERROR_STOP=1 mydns" >/dev/null; }
  db_setup()  {
    db_stop; pg_conftool 17 main set port $PORT; db_start
    su postgres -c "psql -q -p $PORT" <<SQL >/dev/null
DROP DATABASE IF EXISTS mydns;
DROP ROLE IF EXISTS mydns;
CREATE ROLE mydns LOGIN PASSWORD 'mydns';
CREATE DATABASE mydns OWNER mydns;
-- rr.data is BYTEA, which PostgreSQL >= 9.0 returns hex-encoded; mydns expects the old
-- escape format (a pre-existing PostgreSQL backend bug, not what this test is about).
ALTER DATABASE mydns SET bytea_output = 'escape';
SQL
  }
  ;;
*) echo "DBTYPE must be mysql or pgsql"; exit 2 ;;
esac

db_setup
cat > /etc/mydns-outage.conf <<CONF
db-host = 127.0.0.1:$PORT
db-user = mydns
db-password = mydns
database = mydns
db-timeout = $TIMEOUT
db-retry-interval = $RETRY
listen = 127.0.0.1
user = nobody
group = nogroup
log = stderr
pidfile = /tmp/mydns-outage.pid
CONF
src/mydns/mydns -c /etc/mydns-outage.conf --create-tables 2>/dev/null | db_sql || { echo "OUTAGE FAIL: create-tables"; exit 1; }
if [ "$DBTYPE" = pgsql ]; then
  su postgres -c "psql -q -p $PORT mydns -c 'GRANT ALL ON ALL TABLES IN SCHEMA public TO mydns; GRANT ALL ON ALL SEQUENCES IN SCHEMA public TO mydns;'" >/dev/null
fi
db_sql <<'SQL'
INSERT INTO soa (origin,ns,mbox,serial,refresh,retry,expire,minimum,ttl) VALUES ('example.test.','ns1.example.test.','admin.example.test.',1,28800,7200,604800,86400,3600);
INSERT INTO rr (zone,name,type,data,aux,ttl) VALUES ((SELECT id FROM soa WHERE origin='example.test.'),'www','A','192.0.2.10',0,3600);
SQL

pkill -x mydns; sleep 0.3
(setsid src/mydns/mydns -c /etc/mydns-outage.conf > "$LOG" 2>&1 &)
for i in $(seq 40); do dig +short +time=1 +tries=1 @127.0.0.1 www.example.test A | grep -qx 192.0.2.10 && break; sleep 0.25; done

# q <name>: prints "<status> <seconds>"
q() {
  local t0 t1 out st
  t0=$(date +%s.%N)
  out=$(dig +time=15 +tries=1 @127.0.0.1 "$1" A 2>&1)
  t1=$(date +%s.%N)
  st=$(grep -o 'status: [A-Z]*' <<<"$out" | cut -d' ' -f2); [ -n "$st" ] || st=TIMEOUT
  awk -v s="$st" -v a="$t0" -v b="$t1" 'BEGIN { printf "%s %.1f\n", s, b - a }'
}
# expect <description> <status> <max seconds> <name>
expect() {
  local r st sec
  r=$(q "$4"); st=${r% *}; sec=${r#* }
  if [ "$st" = "$2" ] && awk -v a="$sec" -v b="$3" 'BEGIN { exit !(a <= b) }'; then
    echo "OUTAGE PASS $1 ($st in ${sec}s)"
  else
    echo "OUTAGE FAIL $1: got $st in ${sec}s, want $2 within ${3}s"; fail=1
    dig +time=2 +tries=1 @127.0.0.1 "$4" A 2>&1 | grep -v '^;; \(global\|WHEN\|SERVER\|Query time\)\|^$' | sed 's/^/      /' | head -8
  fi
}
# recover <description> <name>: answers again within db-retry-interval (+ db-timeout + slack)
recover() {
  local i r limit=$((RETRY + TIMEOUT + 3)) t0=$(date +%s)
  for i in $(seq 40); do
    r=$(q "$2"); [ "${r% *}" = NOERROR ] || [ "${r% *}" = NXDOMAIN ] && break; sleep 0.5
  done
  if [ $(( $(date +%s) - t0 )) -le $limit ] && { [ "${r% *}" = NOERROR ] || [ "${r% *}" = NXDOMAIN ]; }; then
    echo "OUTAGE PASS $1 (after $(( $(date +%s) - t0 ))s)"
  else
    echo "OUTAGE FAIL $1: still ${r% *} after $(( $(date +%s) - t0 ))s (limit ${limit}s)"; fail=1
  fi
}

expect "baseline A"                 NOERROR  1 www.example.test
expect "baseline NXDOMAIN"          NXDOMAIN 1 before.example.test

echo "-- database hangs"
kill -STOP $(db_pids)
expect "first uncached query"       SERVFAIL $((TIMEOUT + 2)) hang1.example.test
expect "cached name still answers"  NOERROR  1 www.example.test
expect "uncached fails at once"     SERVFAIL 1 hang2.example.test
expect "another zone fails at once" SERVFAIL 1 www.other.test
kill -CONT $(db_pids)
recover "answers again after the hang" thaw.example.test

echo "-- database stopped"
db_stop
expect "refused: uncached fails"    SERVFAIL $((TIMEOUT + 2)) down1.example.test
expect "refused: cached answers"    NOERROR  1 www.example.test
db_start
recover "answers again after restart" up.example.test
expect "lookups work normally"      NXDOMAIN 1 after.example.test

pkill -x mydns; sleep 0.3
n_down=$(grep -c 'database connection failed' "$LOG"); n_up=$(grep -c 'database connection restored' "$LOG")
n_noise=$(grep -c 'error loading SOA\|error finding' "$LOG")
[ "$n_down" = 2 ] && [ "$n_up" = 2 ] && echo "OUTAGE PASS log: one failed/restored line per outage" \
  || { echo "OUTAGE FAIL log: $n_down 'failed', $n_up 'restored' lines (want 2 each)"; fail=1; }
[ "$n_noise" = 0 ] && echo "OUTAGE PASS log: no per-query errors" \
  || { echo "OUTAGE FAIL log: $n_noise per-query error lines"; fail=1; }
grep -q 'runtime error\|ERROR: AddressSanitizer' "$LOG" && { echo "OUTAGE FAIL sanitizer report in $LOG"; fail=1; }

# ok <description> <command...>: PASS if the command succeeds
ok() { local d=$1; shift; if "$@"; then echo "OUTAGE PASS $d"; else echo "OUTAGE FAIL $d"; fail=1; fi; }
# mydns processes that are alive (the dev container's init doesn't reap zombies): "pid ppid" lines
alive() { ps -C mydns -o pid=,ppid=,stat= | awk '$3 !~ /Z/ { print $1, $2 }'; }
running() { [ -n "$(alive)" ]; }
master_pid() { alive | awk '$2 == 1 { print $1; exit }'; }
child_of() { alive | awk -v m="$1" '$2 == m { print $1; exit }'; }
gone_within() { local i; for i in $(seq $(( $1 * 4 ))); do running || return 0; sleep 0.25; done; return 1; }
SLOG=/t/db-startup-$DBTYPE.log

echo "-- database not there at startup"
db_stop
(setsid src/mydns/mydns -c /etc/mydns-outage.conf > "$SLOG" 2>&1 &)
sleep 3
ok "waits for the database instead of exiting" running
ok "logs the retries" grep -q 'retrying in' "$SLOG"
db_start
for i in $(seq 60); do dig +short +time=1 +tries=1 @127.0.0.1 www.example.test A | grep -qx 192.0.2.10 && break; sleep 0.25; done
expect "answers once the database is up" NOERROR 1 www.example.test
ok "logs when it got connected" grep -q 'connected to the database after' "$SLOG"

echo "-- server process restarted while the database is down"
db_stop
master=$(master_pid); child=$(child_of "$master")
kill -9 "$child"; sleep 4
n_died=$(grep -c 'Server pid .* died' "$SLOG")
ok "the master restarted it once, no fork loop ($n_died restart(s))" test "$n_died" -le 1
ok "the new server process waits for the database" test -n "$(child_of "$master")"
kill -TERM "$master"
ok "SIGTERM stops it while it waits" gone_within 5
db_start

echo "-- db-connect-wait gives up"
db_stop
sed -i '/^db-connect-wait/d' /etc/mydns-outage.conf; echo "db-connect-wait = 3" >> /etc/mydns-outage.conf
t0=$(date +%s); src/mydns/mydns -c /etc/mydns-outage.conf >> "$SLOG" 2>&1; rc=$?; t1=$(date +%s)
ok "exits with an error after about 3 s (rc=$rc, $((t1 - t0)) s)" test $rc -ne 0 -a $((t1 - t0)) -ge 2 -a $((t1 - t0)) -le 6
ok "says why" grep -q 'gave up after' "$SLOG"
sed -i '/^db-connect-wait/d' /etc/mydns-outage.conf
db_start
grep -q 'runtime error\|ERROR: AddressSanitizer' "$SLOG" && { echo "OUTAGE FAIL sanitizer report in $SLOG"; fail=1; }

if [ "$DBTYPE" = mysql ]; then
  echo "-- failover between two servers (db-host = primary, second)"
  PORT2=3308 D2=/tmp/mydns-db2 FLOG=/t/db-failover-$DBTYPE.log
  db2()       { mysql --socket=$D2.sock "$@"; }
  db2_start() { (mariadbd --no-defaults --user=mysql --datadir=$D2 --port=$PORT2 --bind-address=127.0.0.1 \
                  --socket=$D2.sock --pid-file=$D2.pid >/dev/null 2>&1 &)
                for i in $(seq 30); do mysqladmin --socket=$D2.sock ping >/dev/null 2>&1 && return; sleep 1; done
                echo "second mariadbd didn't start"; exit 2; }
  db2_stop()  { mysqladmin --socket=$D2.sock shutdown >/dev/null 2>&1; }
  db1_stop()  { mysqladmin shutdown >/dev/null 2>&1; for i in $(seq 30); do mysqladmin ping >/dev/null 2>&1 || return; sleep 0.5; done; }

  db2_stop; rm -rf $D2
  mariadb-install-db --no-defaults --user=mysql --datadir=$D2 >/dev/null 2>&1 || { echo "mariadb-install-db failed"; exit 2; }
  db2_start
  db2 -e "CREATE DATABASE mydns; CREATE USER mydns@127.0.0.1 IDENTIFIED BY 'mydns'; GRANT ALL ON mydns.* TO mydns@127.0.0.1;"
  mysqldump mydns | db2 mydns
  # Names only the second server has, to tell which one answered
  db2 mydns -e "INSERT INTO rr (zone,name,type,data,aux,ttl) SELECT id,'on2','A','192.0.2.22',0,3600 FROM soa;
                INSERT INTO rr (zone,name,type,data,aux,ttl) SELECT id,'stay','A','192.0.2.23',0,3600 FROM soa;
                INSERT INTO rr (zone,name,type,data,aux,ttl) SELECT id,'chk','A','192.0.2.24',0,3600 FROM soa;"

  sed "s/^db-host = .*/db-host = 127.0.0.1:$PORT, 127.0.0.1:$PORT2/" /etc/mydns-outage.conf > /etc/mydns-failover.conf
  pkill -x mydns; sleep 0.3
  (setsid src/mydns/mydns -c /etc/mydns-failover.conf > "$FLOG" 2>&1 &)
  for i in $(seq 40); do dig +short +time=1 +tries=1 @127.0.0.1 www.example.test A | grep -qx 192.0.2.10 && break; sleep 0.25; done
  expect "baseline on the primary"            NOERROR  1 www.example.test
  expect "the primary lacks the second's names" NXDOMAIN 1 chk.example.test

  db1_stop
  expect "primary stopped: fails over at once" NOERROR 2 on2.example.test
  expect "and keeps using the second"         NXDOMAIN 1 f1.example.test
  db_start
  expect "no failback while the second works" NOERROR  1 stay.example.test

  kill -STOP $(cat $D2.pid)
  expect "second hangs: back to the primary"  NXDOMAIN $((TIMEOUT + 2)) f2.example.test
  expect "and stays there"                    NXDOMAIN 1 f3.example.test
  kill -CONT $(cat $D2.pid)

  pkill -x mydns; sleep 0.3
  ok "logs the switch to the second"  grep -q "using database server 127.0.0.1:$PORT2" "$FLOG"
  ok "logs the switch back"           grep -q "using database server 127.0.0.1:$PORT\$" "$FLOG"
  ok "no outage: queries never failed for db-retry-interval" test "$(grep -c 'database connection failed' "$FLOG")" = 0
  grep -q 'runtime error\|ERROR: AddressSanitizer' "$FLOG" && { echo "OUTAGE FAIL sanitizer report in $FLOG"; fail=1; }
  db2_stop
fi

[ $fail = 0 ] && echo "== OUTAGE OK ($DBTYPE) ==" || { echo "== OUTAGE FAILURES ($DBTYPE) =="; sed 's/^/    log: /' "$LOG" | tail -15; }
exit $fail
