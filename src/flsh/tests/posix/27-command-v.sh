command -v cd
command -v echo
command -v test
command -v true
command -v pwd
command -v read
command -v eval
command -v exit
command -v export
command -v unset
command -v nonexistent_cmd_zzz; echo "nonexist nz: $([ $? -ne 0 ] && echo y)"
command -v ls | grep -q '^/' && echo "ls is absolute path"
command -v /bin/sh
command -v /bin/ls
command -v /nonexistent/zzz; echo "nz: $([ $? -ne 0 ] && echo y)"
f() { :; }
command -v f
alias al='echo x'
command -v al
unalias al
command -v al; echo "nz: $([ $? -ne 0 ] && echo y)"
command -v if >/dev/null; echo "keyword st=$?"
command echo via-command
command printf '%s\n' via-command-printf
command -- echo dashdash 2>/dev/null
echo() { printf 'shadow:%s\n' "$*"; }
echo hi
command echo hi
unset -f echo
echo hi
cd() { printf 'fn cd\n'; }
cd /
command cd / && echo "real cd ok"
unset -f cd
command -v cd
PATH=/nonexistent
command -v ls; echo "nz: $([ $? -ne 0 ] && echo y)"
command -v echo
command -v /bin/ls
PATH=/bin:/usr/bin
command -v ls | grep -q 'ls$' && echo "found ls"
hash -r 2>/dev/null
type echo >/dev/null 2>&1; echo "type echo st=$?"
type nonexistent_zzz >/dev/null 2>&1; echo "type missing st nonzero: $([ $? -ne 0 ] && echo y)"
f2() { command -v f2; }
f2
true() { echo overridden; return 7; }
true; echo "st=$?"
command true; echo "st=$?"
unset -f true
true; echo "st=$?"
