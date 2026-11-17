#!/bin/bash
# flow.sh RUN [ENV...] : one login flow through the real window: start freebws on the client page
cd "$(dirname "$0")"
run=$1; shift
curl -s localhost:8080/reset >/dev/null
./g.py "pkill -u tester -x freebws; pkill -u tester -x WPEWebProcess; pkill -u tester -x WPENetworkProc; pkill -u tester -x bwrap; sleep 2; rm -f /home/tester/.cache/freebws/log /tmp/fb.out; su -s /bin/sh tester -c 'cd; set -a; . /tmp/sessenv; $* FREEBWS_DEBUG=1 G_MESSAGES_DEBUG=all setsid ${FBBIN:-freebws} http://client.test:8080/?run=$run </dev/null >/tmp/fb.out 2>&1 &'" >/dev/null
sleep 12
./shot.sh x >/dev/null; ./g.py --get /tmp/s.png f$run-0.png >/dev/null
