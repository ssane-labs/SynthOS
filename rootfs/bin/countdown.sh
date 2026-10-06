# countdown.sh [seconds] - a tiny script with arguments, loops and arithmetic
n=$1
if test -z "$n"
    n=5
end
while test $n -gt 0
    echo -m "{accent}$n{/}..."
    let n = n - 1
    sleep 1
end
echo -m "{ok}Lift off!{/}"
beep 1200 200
