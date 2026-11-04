#!/bin/sh
# Entrypoint for the MyDNS image.
#  - writes /etc/mydns.conf from MYDNS_* variables, unless one is mounted;
#  - waits for the database;
#  - with MYDNS_INIT_SCHEMA=1, creates the tables (mydns --create-tables) if there are none yet,
#    then loads /docker-entrypoint.d/*.sql once;
#  - runs mydns in the foreground (arguments starting with "-" go to mydns).
set -eu

CONF=/etc/mydns.conf

if [ ! -e "$CONF" ]; then
	umask 077
	cat > "$CONF" <<CONF
# Written by docker-entrypoint.sh from MYDNS_* variables. Mount your own $CONF to replace it.
db-host = ${MYDNS_DB_HOST:-db}
db-user = ${MYDNS_DB_USER:-mydns}
db-password = ${MYDNS_DB_PASSWORD:-}
database = ${MYDNS_DATABASE:-mydns}
user = nobody
group = nogroup
listen = ${MYDNS_LISTEN:-*}
log = stderr
pidfile = /run/mydns.pid
allow-tcp = ${MYDNS_ALLOW_TCP:-yes}
allow-axfr = ${MYDNS_ALLOW_AXFR:-no}
allow-update = ${MYDNS_ALLOW_UPDATE:-no}
recursive = ${MYDNS_RECURSIVE:-}
${MYDNS_EXTRA:-}
CONF
	umask 022
fi

# The database settings, read back from the config so a mounted one is used as it is.
conf() { sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\([^#]*\).*/\1/p" "$CONF" | tail -1 | sed 's/[[:space:]]*$//'; }
db_host=$(conf db-host); db_user=$(conf db-user); db_name=$(conf database)
MYSQL_PWD=$(conf db-password); export MYSQL_PWD
db_port=3306
case "$db_host" in *:*) db_port=${db_host#*:}; db_host=${db_host%%:*} ;; esac
sql() { mariadb --skip-ssl-verify-server-cert -h "${db_host:-localhost}" -P "$db_port" -u "$db_user" "$@"; }

case "${1:-mydns}" in -*) set -- mydns "$@" ;; esac

# Only the server needs the database; "mydns --version", "--create-tables" etc. don't wait for it.
server=0
if [ "${1:-}" = mydns ]; then
	server=1
	for a in "$@"; do
		case "$a" in --version|--help|--create-tables|--dump-config) server=0 ;; esac
	done
fi

if [ "$server" = 1 ]; then
	i=0
	until sql -e 'SELECT 1' "$db_name" >/dev/null 2>&1; do
		i=$((i + 1))
		if [ "$i" -ge "${MYDNS_DB_WAIT:-60}" ]; then
			echo "docker-entrypoint: database $db_name on $db_host:$db_port not reachable" >&2
			sql -e 'SELECT 1' "$db_name" >&2 || true
			exit 1
		fi
		sleep 1
	done

	if [ "${MYDNS_INIT_SCHEMA:-0}" = 1 ]; then
		soa_table=$(conf soa-table); soa_table=${soa_table:-soa}
		exists=$(sql -N -e "SELECT COUNT(*) FROM information_schema.tables
			WHERE table_schema = DATABASE() AND table_name = '$soa_table'" "$db_name")
		if [ "$exists" = 0 ]; then
			echo "docker-entrypoint: creating the tables in $db_name"
			mydns --create-tables | sql "$db_name"
			for f in /docker-entrypoint.d/*.sql; do
				[ -e "$f" ] || continue
				echo "docker-entrypoint: loading $f"
				sql "$db_name" < "$f"
			done
		fi
	fi
fi
unset MYSQL_PWD

exec "$@"
