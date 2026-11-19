# $1 delayA $2 delayB $3 loss $4 jitter $5 frames
pkill -f proxy.py; sleep 0.5
(python3 "$(dirname "$0")/udp_proxy.py" 30001 $1 $3 $4 >/dev/null 2>&1 &) ; (python3 "$(dirname "$0")/udp_proxy.py" 30002 $2 $3 $4 >/dev/null 2>&1 &); sleep 1; rm -f *.frames
(timeout 150 ./kc_test 127.0.0.1 30001 Alice create sonic $5 3 > alice.log 2>&1 &) ; sleep 1
timeout 150 ./kc_test 127.0.0.1 30002 Bob join sonic $5 3 > bob.log 2>&1; sleep 3
grep -v '^\[' alice.log | grep -v 'joined\|created\|in room\|starting'; echo ----; grep -v '^\[' bob.log | grep -v 'joined\|created\|in room\|starting'
cmp Alice.frames Bob.frames && echo SAME; wc -l Alice.frames Bob.frames | head -2
pkill -f proxy.py
