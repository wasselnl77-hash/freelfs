set -- a b c
echo "$#"
true; echo "$?"
false; echo "$?"
echo "$?"
(exit 42); echo "$?"
f() { echo "$#:$1"; }
f x y
echo "$#"
echo "$$" | grep -q '^[0-9][0-9]*$' && echo "pid numeric"
sh -c 'echo $PPID' | grep -q '^[0-9][0-9]*$' && echo "ppid numeric"
[ "$$" = "$$" ] && echo "same"
echo "${#}"
sleep 0.01 &
echo "$!" | grep -q '^[0-9][0-9]*$' && echo "bang numeric"
wait
echo "${0:+nonempty-zero}"
x=1
echo "${x}${x}"
echo "$x$x"
echo "$1$2"
echo "${1}0"
echo "$10"
set -- a b c d e f g h i j
echo "$10" "${10}"
cd /
echo "$PWD"
cd /
[ -n "$IFS" ] && echo "IFS default nonempty"
printf '%s' "$IFS" | od -An -c | tr -s ' '
echo "$OPTIND"
a=$?; echo "a=$a"
true | false; echo "$?"
! true; echo "$?"
if false; then :; fi; echo "$?"
f() { return 5; }; f; echo "$?"
f; g=$?; echo "$g"
echo "$#"
set --; echo "$#"
echo "$*"
echo "[$*]"
echo "[$@]"
