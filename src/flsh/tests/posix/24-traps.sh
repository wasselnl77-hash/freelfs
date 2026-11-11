( trap 'echo exit-trap' EXIT; echo body )
( trap 'echo exit-trap-after-exit' EXIT; exit 3 ); echo "st=$?"
( trap 'echo t1' EXIT; trap 'echo t2' EXIT; echo body )
( trap 'echo gone' EXIT; trap - EXIT; echo body )
( trap '' EXIT; echo empty-exit-trap )
trap 'echo got-usr1' USR1
kill -USR1 $$
echo after-usr1
kill -USR1 $$
trap - USR1
trap 'echo got-usr2' USR2
kill -USR2 $$
trap '' USR2
kill -USR2 $$
echo "ignored usr2 survived"
trap - USR2
trap 'echo counted' USR1
for i in 1 2 3; do kill -USR1 $$; done
trap - USR1
# trap with status preserved
trap 'echo in-trap' USR1
false; kill -USR1 $$; echo "st after trap: $?"
trap - USR1
# trap handler sees variables
v=1
trap 'echo "v=$v"' USR1
v=2
kill -USR1 $$
trap - USR1
# trap with numeric signal
trap 'echo num-trap' 10 2>/dev/null
kill -USR1 $$ 2>/dev/null
trap - USR1
# trap in function
f() { trap 'echo from-fn-trap' USR1; }
f
kill -USR1 $$
trap - USR1
# inherited ignore through subshell/child
trap '' USR1
( kill -USR1 $$ ) 2>/dev/null
echo "survived sub-kill"
trap - USR1
# exit trap and exit status
( trap 'echo cleanup' EXIT; false ); echo "st=$?"
( trap 'exit 5' EXIT; true ); echo "st=$?"
( trap 'echo outer' EXIT; ( trap 'echo inner' EXIT; echo body ) )
# subshell resets traps (non-ignored) in subshell
trap 'echo parent-usr1' USR1
( kill -USR1 $$ ) >/dev/null 2>&1
trap - USR1
# trap -p style listing is not compared; only use traps
trap 'echo hup' HUP
kill -HUP $$
trap - HUP
trap 'echo term' TERM
kill -TERM $$
trap - TERM
# trap command with quotes
trap 'echo "quoted  trap"' USR1
kill -USR1 $$
trap - USR1
# trap action string with multiple commands
trap 'echo m1; echo m2' USR1
kill -USR1 $$
trap - USR1
# trap during sleep in background wait
trap 'echo wait-interrupted' USR1
sleep 0.2 &
pid=$!
kill -USR1 $$
wait $pid
echo "waited"
trap - USR1
# EXIT trap in main shell
trap 'echo main-exit-trap' EXIT
echo "end of script"
