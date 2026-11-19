# Usage: udp_proxy.py <listen port> <delay ms> <loss 0..1> [jitter ms]
# Between a client and EmuLinker-K at 127.0.0.1:27888: delays, drops and reorders UDP packets,
# rewrites the HELLOD00D port so the client keeps talking through the proxy.
import socket, sys, random, time, heapq, select
lp, delay, loss, jitter = int(sys.argv[1]), float(sys.argv[2])/1000, float(sys.argv[3]), float(sys.argv[4])/1000 if len(sys.argv)>4 else 0
srv = ('127.0.0.1', 27888)
cs = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); cs.bind(('127.0.0.1', lp))
ss = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ss.bind(('127.0.0.1', 0))
client = None; q = []; seq = 0
while True:
    t = time.time()
    while q and q[0][0] <= t:
        _, _, sock, data, dst = heapq.heappop(q); sock.sendto(data, dst)
    to = max(0, q[0][0] - time.time()) if q else 0.05
    r, _, _ = select.select([cs, ss], [], [], min(to, 0.05))
    for s in r:
        data, a = s.recvfrom(65536)
        if s is cs:
            client = a; dst = srv; out = ss
        else:
            if data.startswith(b'HELLOD00D'): data = b'HELLOD00D%d\x00' % lp
            dst = client; out = cs
        if random.random() < loss: continue
        seq += 1
        heapq.heappush(q, (time.time() + delay/2 + random.random()*jitter, seq, out, data, dst))
