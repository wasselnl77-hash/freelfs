#!/usr/bin/env python3
# A stand-in NTP server for the tests: answers on a UDP port with a time that is
# OFFSET seconds ahead of this machine's. Usage: fakentp.py PORT OFFSET [COUNT]
import socket, struct, sys, time
port, offset = int(sys.argv[1]), float(sys.argv[2])
count = int(sys.argv[3]) if len(sys.argv) > 3 else 1000
NTP = 2208988800
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", port))
def ts(t):
    t += NTP
    return struct.pack(">II", int(t), int((t - int(t)) * 2**32))
for _ in range(count):
    data, addr = s.recvfrom(512)
    rx = time.time() + offset
    if len(data) < 48:
        continue
    reply = struct.pack(">BBbb", 0x24, 2, 4, -20) + b"\0" * 12 + ts(rx) + data[40:48] + ts(rx) + ts(time.time() + offset)
    s.sendto(reply, addr)
