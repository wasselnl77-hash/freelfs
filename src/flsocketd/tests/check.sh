#!/bin/sh
# flsocketd: the daemon is not running until the first connection, gets fd 3 and
# UPSTART_FDS, and is started again after it exits.
set -e
bin=$1
d=$(mktemp -d)
trap 'kill $pid 2>/dev/null; rm -rf $d' EXIT
cat > $d/daemon.py <<'PY'
import os, socket, sys
fd = int(os.environ["UPSTART_FDS"]); assert os.environ["UPSTART_EVENTS"] == "socket"
s = socket.socket(fileno=fd)
open(os.environ["MARK"] + "/started.%d" % os.getpid(), "w").close()
c, _ = s.accept()
c.sendall(b"hello %d\n" % os.getpid())
c.close()
PY
MARK=$d $bin $d/sock -- python3 $d/daemon.py &
pid=$!
sleep 0.5
[ -S $d/sock ] || { echo FAIL socket; exit 1; }
[ "$(ls $d | grep -c started)" = 0 ] || { echo FAIL started early; exit 1; }
a=$(python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.connect('$d/sock');print(s.recv(99).decode().split()[1])")
sleep 2
b=$(python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.connect('$d/sock');print(s.recv(99).decode().split()[1])")
[ "$a" != "$b" ] || { echo FAIL not restarted; exit 1; }
echo "flsocketd: PASS (daemons $a, $b)"
