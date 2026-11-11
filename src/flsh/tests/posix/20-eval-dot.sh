root=$(mktemp -d) || exit 1
eval 'echo hello'
eval echo a b c
eval "x=5; echo \$x"
echo $x
cmd='echo $HOME_UNSET_Q'
eval "$cmd"
eval 'echo "quoted  spaces"'
a='echo one'; b='echo two'
eval "$a; $b"
eval 'f() { echo from-eval-fn; }'
f
eval ''
echo "empty eval: $?"
eval false; echo "false eval: $?"
eval 'exit_in_eval() { return 4; }; exit_in_eval'; echo "st=$?"
v='a b'
eval "set -- $v"
echo "$# $1"
eval "set -- \"$v\""
echo "$# $1"
name=dyn
eval "$name=value"
echo "$dyn"
eval "echo \$$name"
eval "val=\$$name"; echo "$val"
set -- one two
eval 'echo $1 $2'
eval 'echo $#'
eval 'echo "$@"'
eval 'for i in 1 2; do echo e$i; done'
eval 'if true; then echo ifok; fi'
eval 'echo a; echo b' | tr a-z A-Z
eval 'echo out >&2' 2>&1
eval 'cat <<EOF
heredoc in eval $dyn
EOF'
eval "echo 'single'"
eval 'echo $((1+2))'
eval echo '$((2*3))'
eval 'x=$(echo cs); echo $x'
eval 'echo a' 'echo b'
cat > "$root/lib.sh" <<'EOF'
libvar=from-lib
libfn() { echo "libfn $1"; }
echo "sourcing; args: $#"
EOF
. "$root/lib.sh"
echo "$libvar"
libfn z
cat > "$root/args.sh" <<'EOF'
echo "args: $# [$1] [$2]"
EOF
set -- outer1 outer2
. "$root/args.sh"
echo "$1 $2"
cat > "$root/ret.sh" <<'EOF'
echo before-return
return 3
echo not-reached
EOF
f() { . "$root/ret.sh"; echo "in f st=$?"; }
f
cat > "$root/st.sh" <<'EOF'
false
EOF
. "$root/st.sh"; echo "dot st=$?"
cat > "$root/empty.sh" <<'EOF'
EOF
false; . "$root/empty.sh"; echo "empty dot st=$?"
cat > "$root/mod.sh" <<'EOF'
modified=yes
cd "$root"
EOF
( . "$root/mod.sh"; echo "sub: $modified" )
echo "[${modified-unset}]"
( . "$root/nonexistent.sh" ) 2>/dev/null; echo "missing dot nonzero: $([ $? -ne 0 ] && echo y)"
mkdir "$root/bin"
cat > "$root/bin/lib2.sh" <<'EOF'
echo "found via PATH"
EOF
( PATH="$root/bin:$PATH"; . lib2.sh )
cat > "$root/multi.sh" <<'EOF'
for i in 1 2; do
  echo "multi $i"
done
case x in x) echo case-in-dot;; esac
EOF
. "$root/multi.sh"
rm -rf "$root"
