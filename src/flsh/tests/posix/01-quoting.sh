# single, double quotes and backslashes (printf used for anything with backslashes)
p() { printf '%s\n' "$@"; }
p 'a  b   c'
p "a  b   c"
p 'it'\''s'
p "say \"hi\""
p "back\\slash"
p 'back\\slash'
p "dollar \$HOME not expanded"
p 'dollar $HOME'
p "back\`tick"
p "a\bc"
p a\ b\ \ c
x="v w"
p "$x" '$x' "'$x'" '"'"$x"'"'
p "nested: $(echo "inner $(echo 'deep  x')")"
p "$(echo 'single in dq')"
p '$(echo not run)'
p a"b"'c'd
p ""x""
set -- "" ''
echo $#
p "multi
line"
p 'a
b'
p "a\
b"
p 'a\
b'
p \$x \\ \" \'
p "$(printf 'a\\nb')"
p "${x}suffix" "${x:-d}"
p "$((1+2))" '$((1+2))'
q="*"
p "$q" '*'
p "a$(echo 'b c')d"
p "$(echo "a b" | tr ' ' '-')"
p "$(echo '"')"
p "$(echo "\"")"
p "$(printf '%s' '\\')"
