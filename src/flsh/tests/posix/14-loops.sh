i=0
while [ $i -lt 3 ]; do echo w$i; i=$((i+1)); done
i=0
until [ $i -ge 3 ]; do echo u$i; i=$((i+1)); done
for x in a b c; do echo f$x; done
for x in; do echo never; done
echo "empty-for-status: $?"
for x in 1 2 3; do
  [ $x = 2 ] && continue
  echo cont$x
done
for x in 1 2 3; do
  [ $x = 2 ] && break
  echo brk$x
done
for x in a b; do for y in 1 2 3; do
  [ $y = 2 ] && break
  echo $x$y
done; done
for x in a b; do for y in 1 2 3; do
  [ $y = 2 ] && break 2
  echo $x$y
done; done
for x in a b; do for y in 1 2 3; do
  [ $y = 2 ] && continue 2
  echo c$x$y
done; echo not-reached; done
for x in a b c; do for y in 1 2; do for z in p q; do
  [ $z = q ] && continue 3
  echo $x$y$z
done; done; done
i=0
while true; do i=$((i+1)); [ $i -ge 3 ] && break; done; echo "i=$i"
set -- a b c
for x; do echo arg:$x; done
for x in "$@"; do echo q:$x; done
for x in $(echo 1 2 3); do echo cs$x; done
for x in 1 2 3; do :; done; echo "last=$x"
for f in a "b c" d; do echo "[$f]"; done
n=0
while [ $n -lt 5 ]; do
  n=$((n+1))
  case $n in 2) continue;; 4) break;; esac
  echo n$n
done
for x in 1 2 3; do echo $x; done | tr '\n' ' '; echo
while read l; do echo "r:$l"; done <<EOF
a
b
EOF
i=0; while [ $i -lt 2 ]; do i=$((i+1)); done; echo "while-status: $?"
while false; do :; done; echo "while-false-status: $?"
until true; do :; done; echo "until-true-status: $?"
for x in 1 2; do (exit 3); done; echo "for-status: $?"
for x in 1 2; do false; done; echo "for-false-status: $?"
for i in 1 2 3; do if [ $i = 2 ]; then break; fi; done; echo "i=$i"
i=0
while [ $i -lt 3 ]; do
  i=$((i+1))
  j=0
  while [ $j -lt 3 ]; do
    j=$((j+1))
    [ $j = 2 ] && continue 2
    echo "$i.$j"
  done
done
for x in a b c; do case $x in b) break;; esac; echo $x; done
for x in 1 2; do echo $x; done; echo done
for x in *.nonexistent_glob_zz; do echo "$x"; done
for x in "" a; do echo "[$x]"; done
for x in $(printf ''); do echo never; done
i=0
until [ $i -eq 3 ]; do i=$((i+1)); done; echo "i=$i"
