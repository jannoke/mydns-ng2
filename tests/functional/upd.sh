#!/bin/bash
# Start /work's mydns verbosely (DB from the last smoke run) and send one UPDATE.
cd /work; pkill -x mydns; sleep 0.3
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=0
(setsid src/mydns/mydns -v -v > /t/upd.log 2>&1 &)
for i in $(seq 40); do dig +short +time=1 +tries=1 @127.0.0.1 www.example.test A >/dev/null 2>&1 && break; sleep 0.25; done
printf 'server 127.0.0.1\nzone example.test\nupdate add new.example.test 300 A 192.0.2.77\nsend\n' | timeout 8 nsupdate -t 3 2>&1
dig +short @127.0.0.1 new.example.test A
pkill -x mydns; sleep 0.3
grep -iE "UPDATE|refus|error|runtime|SEGV" /t/upd.log | head -12
