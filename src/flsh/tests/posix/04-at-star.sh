show() { echo "$#:$*"; for a in "$@"; do printf '<%s>' "$a"; done; echo; }
show a b c
show "a b" "" c
set -- "a b" "" c
echo "--- \"\$@\""
show "$@"
echo "--- \$@"
show $@
echo "--- \"\$*\""
show "$*"
echo "--- \$*"
show $*
echo "--- IFS=:"
IFS=:
show "$*"
show "$@"
show $*
show $@
IFS=
echo "--- IFS empty"
show "$*"
show "$@"
unset IFS
echo "--- empty args"
set -- "" ""
show "$@"
show "$*"
set --
echo "--- no args"
show "$@"
show "x$@"
show "$@"y
show "$*"
show $@
show $*
echo "--- prefix/suffix"
set -- a b c
show "x$@y"
show "x$*y"
show "pre-$@-post"
IFS=-
show "pre-$*-post"
unset IFS
echo "--- one arg"
set -- "a b"
show "<$@>"
show $@
echo "--- in for"
set -- "x y" z
for i in "$@"; do echo "[$i]"; done
for i in $@; do echo "[$i]"; done
for i in "$*"; do echo "[$i]"; done
for i do echo "[$i]"; done
echo "--- default"
set --
show "${@:-none}"
set -- a "b c"
show "${@:-none}"
show "${*:-none}"
echo "--- IFS multi char"
IFS=' x'
set -- a b c
echo "$*"
IFS=
echo "$*"
unset IFS
echo "--- IFS=, with split"
IFS=,
set -- "a,b" c
show $*
show "$*"
unset IFS
