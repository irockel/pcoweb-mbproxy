"""Shared helpers for the tools: card access (telnet, FTP) and a minimal Modbus TCP client.

Card address and credentials come from the environment:
  PCOWEB_HOST          address of the card
  PCOWEB_USER          telnet / FTP user
  PCOWEB_PASS          telnet / FTP password
"""
import ftplib, io, os, socket, struct, subprocess, sys

D = os.path.dirname(os.path.abspath(__file__))


def env(name):
    v = os.environ.get(name)
    if not v:
        raise SystemExit("set %s (see tools/pcoweb.py)" % name)
    return v


HOST = env("PCOWEB_HOST")


def tn(*cmds):
    """run shell commands on the card via telnet, returns the session text"""
    r = subprocess.run([sys.executable, "-I", os.path.join(D, "tn.py"), HOST, env("PCOWEB_USER"),
                        env("PCOWEB_PASS"), *cmds], capture_output=True, text=True, errors="replace")
    return r.stdout


def ftp():
    f = ftplib.FTP(HOST, timeout=10)
    f.login(env("PCOWEB_USER"), env("PCOWEB_PASS"))
    return f


def fetch(f, path):
    b = io.BytesIO()
    try:
        f.retrbinary("RETR " + path, b.write)
    except ftplib.error_perm:
        return None
    return b.getvalue().decode("latin1")


class Conn:
    """Modbus TCP client: call(fc, addr, n) for reads"""

    def __init__(self, port, unit=1):
        self.s = socket.create_connection((HOST, port), timeout=10)
        self.tid, self.unit = 0, unit

    def call(self, fc, addr, n):
        """returns (payload, None) or (None, exception code); raises on connection problems"""
        self.tid = (self.tid + 1) & 0xFFFF
        self.s.sendall(struct.pack(">HHHBBHH", self.tid, 0, 6, self.unit, fc, addr, n))
        tid, _, length, _ = struct.unpack(">HHHB", self._exact(7))
        pdu = self._exact(length - 1)
        if tid != self.tid:
            raise IOError("tid mismatch %d != %d" % (tid, self.tid))
        if pdu[0] & 0x80:
            return None, pdu[1]
        return pdu[2:], None

    def _exact(self, n):
        b = b""
        while len(b) < n:
            d = self.s.recv(n - len(b))
            if not d:
                raise ConnectionError("closed")
            b += d
        return b

    def close(self):
        self.s.close()


def decode(fc, payload, n):
    """registers (signed) for FC 3/4, bits for FC 1/2"""
    if fc in (3, 4):
        return list(struct.unpack(">%dh" % (len(payload) // 2), payload))
    return [payload[i // 8] >> (i % 8) & 1 for i in range(n)]
