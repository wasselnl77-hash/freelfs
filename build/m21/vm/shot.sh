#!/bin/bash
# shot.sh NAME : screenshot of the guest desktop (snappel --full as tester) into ./NAME.png
cd "$(dirname "$0")"
./g.py "rm -f /tmp/s.png; su -s /bin/sh tester -c 'cd; set -a; . /tmp/sessenv; snappel --full --output /tmp/s.png' >/tmp/snap.out 2>&1; cat /tmp/snap.out; ls -la /tmp/s.png /home/tester/pictures/screenshots 2>&1 | tail -3"
