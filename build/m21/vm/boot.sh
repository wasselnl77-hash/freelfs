#!/bin/bash
# boot.sh ISO|--stick IMG MEM [gl|std] : start the persistent VM, wait for the desktop, session env in /tmp/sessenv
cd "$(dirname "$0")"
(setsid ./startvm.sh "$@" > vm.log 2>&1 &)
for i in $(seq 60); do [ -S ser ] && break; sleep 1; done
sleep 25
for i in $(seq 20); do ./g.py --key ret >/dev/null 2>&1; sleep 2; ./g.py --login 2>/dev/null | grep -q ":~#" && break; done
./g.py -t 150 'for i in $(seq 140); do pgrep -x flgreet-ui && break; sleep 1; done' >/dev/null
sleep 3; ./g.py --key ret >/dev/null
./g.py -t 90 'for i in $(seq 80); do pgrep -x flpanel && break; sleep 1; done' >/dev/null
sleep 3
./g.py "tr '\0' '\n' < /proc/\$(pgrep -u tester -x flpanel | head -1)/environ | grep -E '^[A-Z_]+=[A-Za-z0-9_/:.@,=+-]*\$' > /tmp/sessenv; wc -l < /tmp/sessenv; free -m | head -2"
