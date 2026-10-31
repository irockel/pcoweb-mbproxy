"""Simulated Carel pCO controller as Modbus RTU slave on a pseudo terminal.

Modelled on the controller mbproxy was developed with (a Waterkotte WWPR heat pump): slave 32,
holding registers 0..249, coils 0..199, exception 02 beyond that, one request at a time with a
configurable answer delay (pcod measured 0.27-0.42 s). Faults can be injected per request.
"""
import os
import random
import select
import struct
import threading
import time
import tty


def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return struct.pack("<H", c)


class SlaveSim:
    def __init__(self, slave=32, delay=(0.02, 0.05), max_hr=249, max_coil=199):
        self.slave, self.delay, self.max_hr, self.max_coil = slave, delay, max_hr, max_coil
        self.master, self.slave_fd = os.openpty()
        tty.setraw(self.slave_fd)
        self.path = os.ttyname(self.slave_fd)
        self.hr = [(i * 7) & 0xFFFF for i in range(max_hr + 1)]
        self.coils = [i % 3 == 0 for i in range(max_coil + 1)]
        self.lock = threading.Lock()
        self.requests = []          # (time the request was complete, frame bytes)
        self.answered = []          # time the last answer byte was written
        self.drop = 0               # do not answer the next n requests
        self.corrupt = 0            # answer the next n requests with a broken CRC
        self.exceptions = {}        # address -> exception code for reads/writes touching it
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def close(self):
        self._stop = True
        self._thread.join(2)
        os.close(self.master)
        os.close(self.slave_fd)

    def inject(self, data):
        """write raw bytes towards the gateway (line noise)"""
        os.write(self.master, data)

    def frames(self):
        with self.lock:
            return [f for _, f in self.requests]

    # ------------------------------------------------------------------ serial side
    def _run(self):
        buf = b""
        while not self._stop:
            r, _, _ = select.select([self.master], [], [], 0.05)
            if not r:
                continue
            try:
                buf += os.read(self.master, 512)
            except OSError:
                return
            while True:
                n = self._frame_len(buf)
                if n is None or len(buf) < n:
                    break
                frame, buf = buf[:n], buf[n:]
                if n == 0 or crc16(frame[:-2]) != frame[-2:]:
                    buf = b""       # garbage: resynchronise on the next request
                    break
                if frame[0] != self.slave:
                    continue
                with self.lock:
                    self.requests.append((time.monotonic(), frame))
                self._answer(frame)

    @staticmethod
    def _frame_len(buf):
        if len(buf) < 2:
            return None
        fc = buf[1]
        if 1 <= fc <= 6:
            return 8
        if fc in (15, 16):
            return None if len(buf) < 7 else 9 + buf[6]
        return 0

    def _answer(self, frame):
        time.sleep(random.uniform(*self.delay))
        if self.drop:
            self.drop -= 1
            return
        pdu = self._handle(frame[1:-2])
        out = bytes([self.slave]) + pdu
        out += crc16(out)
        if self.corrupt:
            self.corrupt -= 1
            out = out[:-1] + bytes([out[-1] ^ 0xFF])
        os.write(self.master, out)
        with self.lock:
            self.answered.append(time.monotonic())

    def _handle(self, pdu):
        fc = pdu[0]
        addr, qty = struct.unpack(">HH", pdu[1:5])
        span = range(addr, addr + (1 if fc in (5, 6) else qty))
        for a in span:
            if a in self.exceptions:
                return bytes([fc | 0x80, self.exceptions[a]])
        limit = self.max_coil if fc in (1, 2, 5, 15) else self.max_hr
        if span[-1] > limit:
            return bytes([fc | 0x80, 2])
        if fc in (1, 2):
            bits = self.coils[addr:addr + qty]
            data = bytearray((qty + 7) // 8)
            for i, b in enumerate(bits):
                if b:
                    data[i // 8] |= 1 << (i % 8)
            return bytes([fc, len(data)]) + bytes(data)
        if fc in (3, 4):
            return bytes([fc, 2 * qty]) + struct.pack(">%dH" % qty, *self.hr[addr:addr + qty])
        if fc == 5:
            self.coils[addr] = qty == 0xFF00
            return pdu[:5]
        if fc == 6:
            self.hr[addr] = qty
            return pdu[:5]
        if fc == 15:
            for i in range(qty):
                self.coils[addr + i] = bool(pdu[6 + i // 8] >> (i % 8) & 1)
            return pdu[:5]
        if fc == 16:
            self.hr[addr:addr + qty] = struct.unpack(">%dH" % qty, pdu[6:6 + 2 * qty])
            return pdu[:5]
        return bytes([fc | 0x80, 1])
