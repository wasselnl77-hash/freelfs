#!/bin/bash
# hidden.sh RUN [ENV...] : a page navigates itself while another tab is in front; then the first tab comes back
cd "$(dirname "$0")"
run=$1; shift
curl -s localhost:8080/reset >/dev/null
./g.py "pkill -u tester -x freebws; pkill -u tester -x WPEWebProcess; pkill -u tester -x WPENetworkProc; pkill -u tester -x bwrap; sleep 2; rm -f /home/tester/.cache/freebws/log; su -s /bin/sh tester -c 'cd; set -a; . /tmp/sessenv; $* FREEBWS_DEBUG=1 setsid ${FBBIN:-freebws} http://client.test:8080/autonav?run=$run </dev/null >/tmp/fb.out 2>&1 &'" >/dev/null
sleep 6
./g.py --key ctrl-t >/dev/null
sleep 8
./g.py --key alt-1 >/dev/null
sleep 6
python3 - $run <<'PY'
import json,sys,urllib.request,time
d=json.load(urllib.request.urlopen("http://localhost:8080/status?run="+sys.argv[1]))
hb=[(t,x) for t,s,x in d if s=="heartbeat"]
fr=[int(x.split("frames=")[1]) for t,x in hb[-5:]]
print("welcome loaded:", any(s=="welcome-loaded" for _,s,_ in d), "frames", fr, "age %.1f"%(time.time()-hb[-1][0]), "PAINTING" if fr[-1]>fr[-3]+10 else "STUCK")
PY
