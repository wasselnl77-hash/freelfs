true && echo A
false && echo B
true || echo C
false || echo D
true && true && echo E
true && false || echo F
false || true && echo G
false && true || echo H
true || false && echo I
false || false || echo J
true && echo K || echo L
false && echo K2 || echo L2
! true || echo M
! false && echo N
if true; then echo t1; fi
if false; then echo t2; fi
echo "if-status: $?"
if false; then echo a; elif true; then echo b; else echo c; fi
if false; then echo a; elif false; then echo b; else echo c; fi
if false; then echo a; elif false; then echo b; fi
echo "no-branch-status: $?"
if true; then false; fi; echo "inner-status: $?"
if true
then
  echo multi
  echo line
fi
if
  false
  true
then echo compound-cond
fi
if { false; true; }; then echo brace-cond; fi
if (exit 1); then echo no; else echo sub-else; fi
if [ 1 -eq 1 ] && [ 2 -eq 2 ]; then echo both; fi
if [ 1 -eq 2 ] || [ 2 -eq 2 ]; then echo either; fi
if echo cond-output; then echo then; fi
if false; then :; else echo else-only; fi
x=
if [ -z "$x" ]; then echo empty; fi
if test -n "$x"; then echo no; else echo nonempty-no; fi
a=1
[ $a = 1 ] && echo one || echo not-one
[ $a = 2 ] && echo two || echo not-two
true &&
echo continued-line
false ||
echo continued-or
true && { echo grp1; echo grp2; }
false || { echo orgrp; }
(true && echo sub-and) || echo no
echo x && echo y &
wait
if true; then if false; then echo no; else echo nested-else; fi; fi
if ! command -v nonexistent_cmd_q >/dev/null 2>&1; then echo not-found; fi
