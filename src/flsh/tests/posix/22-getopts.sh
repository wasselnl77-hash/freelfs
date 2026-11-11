parse() {
  OPTIND=1
  while getopts "ab:c" opt; do
    case $opt in
      a) echo "opt a";;
      b) echo "opt b arg=$OPTARG";;
      c) echo "opt c";;
      '?') echo "unknown opt (OPTARG=${OPTARG-})";;
    esac
  done
  shift $((OPTIND-1))
  echo "rest($#): $*"
}
echo "--- 1"; parse -a -b val -c file1 file2 2>/dev/null
echo "--- 2"; parse -abval rest 2>/dev/null
echo "--- 3"; parse -ac -b "x y" -- -a 2>/dev/null
echo "--- 4"; parse -z 2>/dev/null
echo "--- 5"; parse -b 2>/dev/null
echo "--- 6"; parse a b c
echo "--- 7"; parse -a -- -c
echo "--- 8"; parse -a - b
echo "--- 9"; parse
echo "--- 10"; parse -cab "" tail 2>/dev/null
sparse() {
  OPTIND=1
  while getopts ":ab:" opt; do
    case $opt in
      a) echo "silent a";;
      b) echo "silent b=$OPTARG";;
      '?') echo "silent unknown: $OPTARG";;
      ':') echo "silent missing arg for: $OPTARG";;
    esac
  done
  shift $((OPTIND-1))
  echo "rest: $*"
}
echo "--- s1"; sparse -a -x -b
echo "--- s2"; sparse -b
echo "--- s3"; sparse -ab foo bar
echo "--- s4"; sparse -q -a
echo "--- s5"; sparse -b -a
echo "--- loop with set --"
set -- -a -b arg -c rest
OPTIND=1
while getopts "ab:c" o; do echo "o=$o OPTARG=${OPTARG-} OPTIND=$OPTIND"; done 2>/dev/null
echo "final OPTIND=$OPTIND"
echo "--- status"
OPTIND=1
set -- -a
getopts "a" o; echo "st=$? o=$o"
getopts "a" o; echo "st=$? o=$o"
echo "--- explicit args"
OPTIND=1
while getopts "x:y" o -x 1 -y -x 2; do echo "$o ${OPTARG-}"; done
echo "OPTIND=$OPTIND"
echo "--- opt var unset-ness"
OPTIND=1
set -- -y
getopts ":x" o; echo "o=$o OPTARG=$OPTARG"
echo "--- multiple scans"
OPTIND=1
set -- -a -a
n=0; while getopts a o; do n=$((n+1)); done; echo "n=$n"
OPTIND=1
set -- -a -a -a
n=0; while getopts a o; do n=$((n+1)); done; echo "n=$n"
echo "--- bundled with arg"
OPTIND=1
set -- -abfoo -c
while getopts "ab:c" o; do echo "$o ${OPTARG-}"; done 2>/dev/null
