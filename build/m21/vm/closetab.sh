#!/bin/bash
# closetab.sh RUN [ENV...] : tab 1 shows the client page; a second tab is opened and closed again; does tab 1 still paint?
cd "$(dirname "$0")"
run=$1; shift
curl -s localhost:8080/reset >/dev/null
./g.py "pkill -u tester -x freebws; pkill -u tester -x WPEWebProcess; pkill -u tester -x WPENetworkProc; pkill -u tester -x bwrap; sleep 2; rm -f /home/tester/.cache/freebws/log; su -s /bin/sh tester -c 'cd; set -a; . /tmp/sessenv; $* FREEBWS_DEBUG=1 setsid ${FBBIN:-freebws} http://client.test:8080/?run=$run </dev/null >/tmp/fb.out 2>&1 &'" >/dev/null
sleep 8
./g.py --key ctrl-t >/dev/null; sleep 3
./g.py --key ctrl-t >/dev/null; sleep 3
./g.py --key ctrl-w >/dev/null; sleep 2
./g.py --key ctrl-w >/dev/null; sleep 6
python3 - $run <<'PY'
import json,sys,urllib.request,time
d=json.load(urllib.request.urlopen("http://localhost:8080/status?run="+sys.argv[1]))
hb=[(t,x) for t,s,x in d if s=="heartbeat"]
fr=[int(x.split("frames=")[1].split()[0]) for t,x in hb[-5:]]
print("frames", fr, "age %.1f"%(time.time()-hb[-1][0]), "PAINTING" if fr[-1]>fr[-3]+10 else "STUCK")
PY
