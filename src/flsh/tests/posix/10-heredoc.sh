x=world
cat <<EOF
hello $x
  indented
\$escaped \\ back
`echo bq`
$(echo cs)
$((1+2))
"quotes" 'single'
EOF
cat <<'EOF'
hello $x
\$no `expansion` $(here) \\
EOF
cat <<"EOF"
hello $x \\
EOF
cat <<\EOF
hello $x \\
EOF
cat <<-EOF
	tab stripped
		two tabs
	  tab then spaces
	$x
	EOF
cat <<-'EOF'
	quoted $x
	EOF
cat <<EOF
	leading tab kept
EOF
cat <<EOF
line with trailing backslash \
continued
EOF
cat <<EOF

blank above and below

EOF
cat <<EOF
EOF
echo after-empty
cat <<A; cat <<B
first A
A
first B
B
cat <<A <<B
ignored A
A
used B
B
cat <<EOF | tr a-z A-Z
piped heredoc
EOF
(cat <<EOF
in subshell $x
EOF
)
f() {
cat <<EOF
in function $x $1
EOF
}
f arg
if true; then
	cat <<-EOF
	in if $x
	EOF
fi
while read l; do echo "got:$l"; done <<EOF
l1
l2
EOF
cat <<EOF
${x}suffix ${undef:-dflt} ${x#wo} ${#x}
EOF
cat <<'END OF'
delimiter with space
END OF
cat <<EOF
a\
b
EOF
cat <<EOF
$ alone and $
EOF
cat <<EOF
"$x" '$x'
EOF
y=$(cat <<EOF
inside subst
EOF
)
echo "$y"
cat <<EOF >&2
to stderr
EOF
set -- p1 p2
cat <<EOF
$1 $2 $# "$@"
EOF
cat <<"E"O"F"
quoted partial $x
EOF
cat 0<<EOF
fd0
EOF
