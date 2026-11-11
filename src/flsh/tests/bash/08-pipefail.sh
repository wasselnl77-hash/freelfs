true | true; echo "default: $?"
false | true; echo "default: $?"
true | false; echo "default: $?"
set -o pipefail
true | true; echo "pf: $?"
false | true; echo "pf: $?"
true | false; echo "pf: $?"
true | false | true; echo "pf mid: $?"
false | false | true; echo "pf: $?"
(exit 3) | (exit 0) | (exit 0); echo "pf first: $?"
(exit 3) | (exit 4) | (exit 0); echo "pf rightmost-failure: $?"
(exit 2) | (exit 0) | (exit 5); echo "pf: $?"
echo ok | cat | cat; echo "pf all ok: $?"
! true | false | true; echo "neg pf: $?"
! true | true; echo "neg pf2: $?"
! false | false; echo "neg pf3: $?"
if true | false | true; then echo yes; else echo "if sees failure"; fi
true | false | true || echo "or taken"
x=$(true | false | true); echo "subst st=$?"
set +o pipefail
true | false | true; echo "off: $?"
set -o pipefail
printf 'a\nb\n' | grep -q a | cat; echo "pf: $?"
echo hi | grep -q nomatch | cat; echo "pf nomatch: $?"
echo hi | { cat; false; } | cat; echo "pf brace: $?"
f() { return 7; }
f | cat; echo "pf func: $?"
cat /dev/null | f | cat; echo "pf func mid: $?"
set -e
( true | false | true; echo not-reached ); echo "set -e + pf: $?"
set +e
set +o pipefail
true | false | true; echo "end off: $?"
