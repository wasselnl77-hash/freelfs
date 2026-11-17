#!/bin/bash
# loop.sh N ENV... : N login flows through the real window, one verdict each
cd "$(dirname "$0")"
n=$1; shift
for i in $(seq $n); do
  ./flow.sh $i "$@"
  xy=$(python3 findbtn.py f$i-0.png 11 87 208 150) || { echo "run $i: no button on the page"; continue; }
  ./g.py --click $xy; sleep 9
  if ./verdict.py $i; then echo "run $i: ok"; else echo "run $i: FAIL"; ./shot.sh x >/dev/null; ./g.py --get /tmp/s.png fail$i.png >/dev/null; fi
done
