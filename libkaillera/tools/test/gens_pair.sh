#!/bin/bash
# Two Gens64 under wine (Xvfb :99) through the kailleraclient.dll dialogs to a local
# EmuLinker-K at 127.0.0.1:27888: p1 creates "coloco", p2 joins, p1 starts. Folders
# /tmp/kn/p1 and p2 hold coloco.bin; window positions assume no window manager.
# two Gens64 + kailleraclient.dll against the local EmuLinker; DISPLAY :99
export DISPLAY=:99
DLL=${DLL:-/tmp/kclb-x86_64/kailleraclient.dll}; EXE=${EXE:-/home/claude/gens64/build/w64/Gens.exe}; WP=${WP:-/tmp/wp64}
env -i HOME=/root PATH=/usr/bin:/bin WINEPREFIX=$WP wineserver -k; sleep 2
win() { for i in $(seq 120); do w=$(xdotool search --onlyvisible --name "$1" 2>/dev/null | tail -1); [ -n "$w" ] && { echo $w; return; }; sleep 0.25; done; }
newwin() { for i in $(seq 120); do for w in $(xdotool search --onlyvisible --name "$1" 2>/dev/null); do case " $2 " in *" $w "*) ;; *) echo $w; return;; esac; done; sleep 0.25; done; }
for p in p1 p2; do d=/tmp/kn/$p; cp $DLL $EXE $d/; rm -f $d/kailleraclient.log; printf '[Client]\r\nNick=Iron_%s\r\nDebug=1\r\nCodePage=1251\r\n[Recent]\r\n0=127.0.0.1:27888|Local\r\n' $p > $d/kailleraclient.ini; done
G=""; L=""; R=""
for p in p1 p2; do
  (cd /tmp/kn/$p; env -i HOME=/root PATH=/usr/bin:/bin DISPLAY=:99 WINEPREFIX=$WP WINEDEBUG=-all wine Gens.exe >/tmp/kn/$p.log 2>&1 &)
  g=$(newwin 'Gens64' "$G"); G="$G $g"; sleep 2
  xdotool mousemove 17 41 click 1; sleep 0.7; xdotool mousemove 42 112 click 1
  s=$(win 'Kaillera Client'); xdotool windowmove $s 0 0; sleep 0.5
  xdotool mousemove --window $s 35 83 click 1; sleep 0.3; xdotool mousemove --window $s 215 450 click 1
  l=$(newwin 'Kaillera: ' "$L"); L="$L $l"; sleep 1.5
  if [ $p = p1 ]; then xdotool windowmove $l 0 400; sleep 0.5; xdotool mousemove --window $l 145 463 click 1; sleep 0.7; xdotool key Down Right Return
  else xdotool windowmove $l 640 400; sleep 0.5; xdotool mousemove --window $l 40 311 click --repeat 2 --delay 80 1; fi
  sleep 2
  [ $p = p1 ] && xdotool windowmove $g 960 560 || xdotool windowmove $g 640 560
  sleep 1
done
echo "gens:$G lobby:$L"
l1=$(echo $L | cut -d' ' -f1); xdotool mousemove --window $l1 ${SX:-570} ${SY:-294} click 1
