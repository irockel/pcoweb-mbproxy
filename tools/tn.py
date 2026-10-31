#!/usr/bin/env python3
"""Minimal telnet runner: tn.py HOST USER PASS CMD [CMD...]"""
import socket, sys, time, re

host, user, pw, *cmds = sys.argv[1:]
s = socket.create_connection((host, 23), timeout=5)
s.settimeout(1.5)


def read(wait=1.5):
    buf = b""
    end = time.time() + wait
    while time.time() < end:
        try:
            d = s.recv(4096)
        except socket.timeout:
            if buf:
                break
            continue
        if not d:
            break
        # strip/refuse telnet negotiation
        out = b""
        i = 0
        while i < len(d):
            if d[i] == 255 and i + 2 < len(d):
                cmd, opt = d[i + 1], d[i + 2]
                if cmd == 253:
                    s.sendall(bytes([255, 252, opt]))
                elif cmd == 251:
                    s.sendall(bytes([255, 254, opt]))
                i += 3
            else:
                out += d[i:i + 1]
                i += 1
        buf += out
        end = time.time() + 0.6
    return buf.decode("latin1")


print(read(2.5), end="")
s.sendall((user + "\r\n").encode())
print(read(), end="")
s.sendall((pw + "\r\n").encode())
print(read(2.5), end="")
for c in cmds:
    s.sendall((c + "\r\n").encode())
    print(read(3), end="")
s.close()
