sleep 0.05 &
wait $!
echo "st=$?"
( exit 3 ) &
wait $!
echo "st=$?"
( exit 0 ) &
pid=$!
wait $pid
echo "st=$?"
false &
wait $!; echo "false st=$?"
true &
wait $!; echo "true st=$?"
( sleep 0.05; exit 7 ) &
p1=$!
( sleep 0.01; exit 9 ) &
p2=$!
wait $p1; echo "p1=$?"
wait $p2; echo "p2=$?"
wait; echo "wait-all st=$?"
wait; echo "wait-none st=$?"
x=1
( x=2 ) &
wait
echo "x=$x"
{ echo from-bg-group; } &
wait
echo a &
wait
f() { echo "bg-func"; }
f &
wait
echo $! | grep -q '^[0-9][0-9]*$' && echo "bang is numeric"
sleep 0.02 &
a=$!
sleep 0.02 &
b=$!
[ "$a" != "$b" ] && echo "distinct pids"
wait $a $b; echo "wait two st=$?"
wait 999999 2>/dev/null; echo "wait bad pid st=$?"
( echo one; sleep 0.05; echo three ) &
sleep 0.01
echo two
wait
sleep 5 &
pid=$!
kill $pid
wait $pid 2>/dev/null
echo "killed st=$?"
sleep 5 &
pid=$!
kill -9 $pid
wait $pid 2>/dev/null
echo "kill9 st=$?"
( exit 2 ) & ( exit 4 ) &
wait
echo "after both"
sleep 0.01 && echo "and-list" &
wait
echo bg-pipeline | cat &
wait
cat <<EOF &
bg heredoc
EOF
wait
echo "bg-redirect" > /dev/null &
wait; echo "st=$?"
for i in 1 2 3; do ( exit $i ) & wait $!; echo "i=$i st=$?"; done
(sleep 0.01; echo "nested bg") &
wait
echo end
