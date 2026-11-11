HOME=/home/fake
echo ~
echo ~/x
echo ~/x/y
echo "~"
echo '~'
echo \~
echo a~
echo x=~
x=~
echo $x
x=~/foo:~/bar
echo $x
x=a:~:b
echo $x
y=~
echo "$y"
echo ~root | sed 's/root.*/ROOT/'
HOME=/
echo ~
echo ~/x
HOME=/h/
echo ~
echo ~/x
HOME=
echo "[~]"
HOME=/a
echo ${undefined:-~}
echo ${undefined:-~/z}
echo "${undefined:-~}"
for d in ~ ~/q; do echo $d; done
echo ~+ >/dev/null
echo ~:
echo ~/
echo ~"/x"
set -- ~ ~/a
echo "$1 $2"
HOME="/with space"
echo ~
set -- ~
echo $#
echo ~/a
