true | false; echo $?
false | true; echo $?
true | true; echo $?
! true; echo $?
! false; echo $?
! true | false; echo $?
! false | true; echo $?
if ! false; then echo neg-if; fi
if ! true; then echo no; else echo neg-else; fi
echo a | cat | cat | tr a b
(exit 7) | cat; echo $?
(exit 7) | (exit 8); echo $?
echo x | while read l; do echo "got $l"; done
echo x | { read l; echo "got $l"; }
printf 'a\nb\n' | { n=0; while read l; do n=$((n+1)); done; echo $n; }
x=0
echo hi | { x=1; }
echo "x=$x"
false; echo $?
true; echo $?
( exit 3 ); echo $?
{ (exit 4); }; echo $?
echo foo | grep -q foo; echo $?
echo foo | grep -q bar; echo $?
nonexistent_cmd_xyz 2>/dev/null; echo $?
/nonexistent/path 2>/dev/null; echo $?
: ; echo $?
cd /nonexistent_dir 2>/dev/null; echo "cd-fail nonzero: $([ $? -ne 0 ] && echo y)"
echo $?
echo $?
false || echo $?
sh -c 'exit 5'; echo $?
f() { return 6; }; f; echo $?
f | true; echo $?
! f; echo $?
true && ! false; echo $?
printf 'a\nb\nc\n' | sed -n 2p
yes | head -n 2
printf '%s\n' 3 1 2 | sort | tr '\n' ' '; echo
cat /dev/null | cat; echo $?
echo end
