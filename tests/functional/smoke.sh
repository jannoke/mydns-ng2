#!/bin/bash
# Functional smoke test: MariaDB + test zone + ordinary dig queries against /work's mydns.
# Usage: smoke.sh   (run inside the dev container after build.sh)
DBTYPE=${DBTYPE:-mysql}
if ! mysqladmin ping >/dev/null 2>&1; then (mysqld_safe >/dev/null 2>&1 &); for i in $(seq 30); do mysqladmin ping >/dev/null 2>&1 && break; sleep 1; done; fi
mysql -e "DROP DATABASE IF EXISTS mydns; CREATE DATABASE mydns; CREATE USER IF NOT EXISTS mydns@localhost IDENTIFIED BY 'mydns'; GRANT ALL ON mydns.* TO mydns@localhost;"
cat > /etc/mydns.conf <<CONF
db-host = localhost
db-user = mydns
db-password = mydns
database = mydns
listen = 127.0.0.1
user = nobody
group = nogroup
log = stderr
allow-tcp = yes
allow-axfr = yes
ixfr-enabled = yes
allow-update = yes
CONF
cd /work
src/mydns/mydns --create-tables 2>/dev/null | mysql mydns || { echo "SMOKE FAIL: create-tables"; exit 1; }
mysql mydns -e "ALTER TABLE rr MODIFY data VARBINARY(1024) NOT NULL;
INSERT INTO soa (id,origin,ns,mbox,serial,refresh,retry,expire,minimum,ttl) VALUES (1,'example.test.','ns1.example.test.','admin.example.test.',5,28800,7200,604800,86400,3600);
INSERT INTO rr (zone,name,type,data,aux,ttl) VALUES
 (1,'','NS','ns1.example.test.',0,3600),(1,'ns1','A','127.0.0.1',0,3600),
 (1,'www','A','192.0.2.10',0,3600),(1,'www','AAAA','2001:db8::10',0,3600),
 (1,'','MX','mail.example.test.',10,3600),(1,'mail','A','192.0.2.25',0,3600),
 (1,'alias','CNAME','www.example.test.',0,3600),(1,'short','TXT','hello world',0,3600),
 (1,'_sip._tcp','SRV','0 5060 sip.example.test.',10,3600),
 (1,'long','TXT',REPEAT('x',300),0,3600),
 (1,'multi','TXT',CONCAT(REPEAT('a',200),CHAR(0),REPEAT('b',200)),0,3600);
UPDATE soa SET xfer='127.0.0.1', update_acl='127.0.0.1' WHERE id=1;"
pkill -x mydns; sleep 0.5
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=0:log_path=/t/asan/smoke
mkdir -p /t/asan; chmod 777 /t/asan; rm -f /t/asan/smoke.*
(setsid src/mydns/mydns > /t/server.log 2>&1 &)
for i in $(seq 40); do dig +short +time=1 +tries=1 @127.0.0.1 www.example.test A >/dev/null 2>&1 && break; sleep 0.25; done
fail=0
t() { # t <description> <expected-substring> <dig args...>
  local d=$1 e=$2; shift 2
  local out; out=$(dig +time=2 +tries=1 @127.0.0.1 "$@" 2>&1)
  if grep -qF -- "$e" <<<"$out"; then echo "SMOKE PASS $d"; else echo "SMOKE FAIL $d"; echo "$out" | sed 's/^/    /' | head -20; fail=1; fi
}
t "A"            "192.0.2.10"            +short www.example.test A
t "AAAA"         "2001:db8::10"          +short www.example.test AAAA
t "MX"           "10 mail.example.test." +short example.test MX
t "CNAME"        "www.example.test."     +short alias.example.test A
t "TXT"          '"hello world"'         +short short.example.test TXT
t "SRV"          "10 0 5060 sip.example.test." +short _sip._tcp.example.test SRV
t "SOA"          "ns1.example.test."     +short example.test SOA
t "NXDOMAIN"     "status: NXDOMAIN"      nope.example.test A
t "TCP A"        "192.0.2.10"            +tcp +short www.example.test A
t "AXFR"         "www.example.test."     example.test AXFR
t "long TXT"     "status: NOERROR"       long.example.test TXT
t "multi TXT"    "bbbbbbbbbb"            +short multi.example.test TXT
# DNS UPDATE from loopback (no update_acl column -> loopback only)
printf 'server 127.0.0.1\nzone example.test\nupdate add new.example.test 300 A 192.0.2.77\nupdate add new.example.test 300 TXT "one" "two"\nupdate add new.example.test 300 MX 5 mail.example.test\nsend\n' | nsupdate 2>&1 | sed 's/^/    nsupdate: /'
t "UPDATE add A"   "192.0.2.77"           +short new.example.test A
t "UPDATE add TXT" '"one" "two"'          +short new.example.test TXT
t "UPDATE add MX"  "5 mail.example.test." +short new.example.test MX
printf 'server 127.0.0.1\nzone example.test\nupdate delete new.example.test A\nsend\n' | nsupdate 2>&1 | sed 's/^/    nsupdate: /'
t "UPDATE delete"  "status: NOERROR"      new.example.test A
out=$(dig +short @127.0.0.1 new.example.test A); [ -z "$out" ] && echo "SMOKE PASS UPDATE deleted" || { echo "SMOKE FAIL UPDATE deleted ($out)"; fail=1; }
# IXFR must honour the per-zone xfer ACL, like AXFR (the client here is 127.0.0.1)
t "IXFR allowed"  "example.test."        +noall +answer example.test IXFR=1
mysql mydns -e "UPDATE soa SET xfer='10.9.9.9' WHERE id=1"
t "AXFR denied"   "Transfer failed"      example.test AXFR
t "IXFR denied"   "Transfer failed"      example.test IXFR=1
mysql mydns -e "UPDATE soa SET xfer='127.0.0.1' WHERE id=1"
# Recursive forwarding: a second instance on 127.0.0.2 serves fwd.test from database mydns2;
# the main instance (no fwd.test zone) forwards to it.
mysql -e "DROP DATABASE IF EXISTS mydns2; CREATE DATABASE mydns2; GRANT ALL ON mydns2.* TO mydns@localhost;"
src/mydns/mydns --create-tables 2>/dev/null | mysql mydns2
mysql mydns2 -e "INSERT INTO soa (id,origin,ns,mbox,serial,refresh,retry,expire,minimum,ttl) VALUES (1,'fwd.test.','ns.fwd.test.','admin.fwd.test.',1,28800,7200,604800,86400,3600);
  INSERT INTO rr (zone,name,type,data,aux,ttl) VALUES (1,'host','A','198.51.100.7',0,3600);"
sed -e 's/^database = mydns$/database = mydns2/' -e 's/^listen = .*/listen = 127.0.0.2/' -e '/^pidfile/d' /etc/mydns.conf > /etc/mydns-fwd.conf
echo "pidfile = /tmp/mydns-fwd.pid" >> /etc/mydns-fwd.conf
pkill -x mydns; sleep 0.5
(setsid src/mydns/mydns -c /etc/mydns-fwd.conf > /t/server-fwd.log 2>&1 &)
echo "recursive = 127.0.0.2" >> /etc/mydns.conf
(setsid src/mydns/mydns >> /t/server.log 2>&1 &)
for i in $(seq 40); do dig +short +time=1 +tries=1 @127.0.0.1 www.example.test A >/dev/null 2>&1 && break; sleep 0.25; done
dig +time=1 +tries=1 @127.0.0.1 warmup.fwd.test A >/dev/null 2>&1; dig +tcp +time=1 +tries=1 @127.0.0.1 warmup.fwd.test A >/dev/null 2>&1  # first forward per transport is lost while connecting (pre-existing, see Phase C)
t "recursive UDP" "198.51.100.7"        +short host.fwd.test A
t "recursive size" "MSG SIZE  rcvd: 4"   host.fwd.test A     # exact length, not padded to 512
t "recursive TCP" "198.51.100.7"        +tcp +short host.fwd.test A
t "recursive NX"  "status: NXDOMAIN"     nope.fwd.test A
sed -i '/^recursive = /d' /etc/mydns.conf
pkill -x mydns; sleep 0.3
if ls /t/asan/smoke.* >/dev/null 2>&1 || grep -q "runtime error" /t/server.log /t/server-fwd.log 2>/dev/null; then grep -h "runtime error" /t/server.log /t/server-fwd.log 2>/dev/null | sort -u | head; echo "SMOKE FAIL: sanitizer reports:"; grep -h -A6 'ERROR: \|runtime error' /t/asan/smoke.* | head -40; fail=1; fi
[ $fail = 0 ] && echo "== SMOKE OK ==" || echo "== SMOKE FAILURES =="
exit $fail
