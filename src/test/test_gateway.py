"""End-to-end tests: mbproxy (host build) between Modbus TCP clients and the simulated pCO.

usage: test_gateway.py PATH_TO_MBPROXY_HOST [unittest args]
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from slave_sim import SlaveSim  # noqa: E402

BINARY = None


class ModbusError(Exception):
    def __init__(self, code):
        super().__init__("exception %d" % code)
        self.code = code


class Client:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.tid = 0

    def close(self):
        self.s.close()

    def adu(self, pdu, unit=1, tid=None):
        if tid is None:
            self.tid = (self.tid + 1) & 0xFFFF
            tid = self.tid
        return struct.pack(">HHHB", tid, 0, len(pdu) + 1, unit) + pdu

    def recv_exact(self, n):
        b = b""
        while len(b) < n:
            d = self.s.recv(n - len(b))
            if not d:
                raise ConnectionError("closed by gateway")
            b += d
        return b

    def recv_adu(self):
        tid, proto, length, unit = struct.unpack(">HHHB", self.recv_exact(7))
        return tid, unit, self.recv_exact(length - 1)

    def call(self, pdu, unit=1):
        self.s.sendall(self.adu(pdu, unit))
        tid, u, resp = self.recv_adu()
        assert tid == self.tid and u == unit, (tid, self.tid, u, unit)
        if resp[0] & 0x80:
            raise ModbusError(resp[1])
        return resp

    def read_hr(self, addr, n, unit=1):
        r = self.call(struct.pack(">BHH", 3, addr, n), unit)
        return list(struct.unpack(">%dH" % n, r[2:]))

    def read_coils(self, addr, n):
        r = self.call(struct.pack(">BHH", 1, addr, n))
        return [bool(r[2 + i // 8] >> (i % 8) & 1) for i in range(n)]

    def write_hr(self, addr, value):
        return self.call(struct.pack(">BHH", 6, addr, value))

    def write_hrs(self, addr, values):
        return self.call(struct.pack(">BHHB", 16, addr, len(values), 2 * len(values))
                         + struct.pack(">%dH" % len(values), *values))

    def write_coil(self, addr, on):
        return self.call(struct.pack(">BHH", 5, addr, 0xFF00 if on else 0))


def read(path):
    with open(path) as f:
        return f.read()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class GatewayTest(unittest.TestCase):
    """starts a fresh simulator + gateway per test; subclasses set ARGS / SIM"""
    BASE = ["-s", "32", "-a", "249", "-k", "199"]       # the simulated controller
    ARGS = []
    SIM = {}

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.sim = SlaveSim(**self.SIM)
        self.port = free_port()
        self.wdog = os.path.join(self.tmp.name, "hwio")
        self.stat = os.path.join(self.tmp.name, "stat")
        self.log = os.path.join(self.tmp.name, "log")
        self.start_gateway(self.ARGS)
        self.clients = []

    def start_gateway(self, args):
        cmd = [BINARY, "-d", self.sim.path, "-p", str(self.port), "-W", self.wdog, "-S", self.stat,
               "-l", self.log, "-T", "300", "-g", "20"] + self.BASE + list(args)
        self.proc = subprocess.Popen(cmd)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return
            except OSError:
                if self.proc.poll() is not None:
                    self.fail("gateway exited: %s" % self.proc.returncode)
                time.sleep(0.02)
        self.fail("gateway does not listen")

    def tearDown(self):
        for c in self.clients:
            c.close()
        alive = self.proc.poll() is None
        if alive:
            self.proc.terminate()
        self.proc.wait(5)
        self.sim.close()
        if not alive and not getattr(self, "expect_exit", False):
            self.fail("gateway died during the test")
        if self._outcome_failed():
            print(read(self.log) if os.path.exists(self.log) else "(no log)")
        self.tmp.cleanup()

    def _outcome_failed(self):
        res = getattr(self._outcome, "result", None)
        return res is not None and any(t is self for t, _ in res.failures + res.errors)

    def client(self):
        c = Client(self.port)
        self.clients.append(c)
        return c

    def stats(self):
        return dict(line.split("=") for line in read(self.stat).split())

    def logtext(self):
        return read(self.log)


class ReadTests(GatewayTest):
    def test_read_holding_registers(self):
        c = self.client()
        self.assertEqual(c.read_hr(1, 10), self.sim.hr[1:11])
        self.assertEqual(c.read_hr(125, 125), self.sim.hr[125:250])

    def test_read_coils(self):
        self.assertEqual(self.client().read_coils(0, 20), self.sim.coils[:20])

    def test_rtu_frame_uses_slave_32_and_unit_id_is_echoed(self):
        c = self.client()
        self.assertEqual(c.read_hr(37, 2, unit=0xFF), self.sim.hr[37:39])
        self.assertEqual(self.sim.frames()[0][:6], bytes([32, 3, 0, 37, 0, 2]))

    def test_out_of_range_is_rejected_without_bus_traffic(self):
        c = self.client()
        with self.assertRaises(ModbusError) as e:
            c.read_hr(240, 20)
        self.assertEqual(e.exception.code, 2)
        with self.assertRaises(ModbusError) as e:
            c.read_hr(0, 126)
        self.assertEqual(e.exception.code, 3)
        with self.assertRaises(ModbusError) as e:
            c.read_coils(195, 10)
        self.assertEqual(e.exception.code, 2)
        with self.assertRaises(ModbusError) as e:
            c.call(bytes([8, 0, 0, 0, 0]))
        self.assertEqual(e.exception.code, 1)
        self.assertEqual(self.sim.frames(), [])

    def test_slave_exception_is_passed_through_without_retry(self):
        self.sim.exceptions[100] = 2
        with self.assertRaises(ModbusError) as e:
            self.client().read_hr(99, 3)
        self.assertEqual(e.exception.code, 2)
        self.assertEqual(len(self.sim.frames()), 1)

    def test_split_and_pipelined_requests(self):
        c = self.client()
        a1 = c.adu(struct.pack(">BHH", 3, 1, 2), tid=100)
        a2 = c.adu(struct.pack(">BHH", 3, 300, 1), tid=101)     # rejected locally
        a3 = c.adu(struct.pack(">BHH", 3, 10, 1), tid=102)
        data = a1 + a2 + a3
        for i in range(0, len(data), 5):            # odd chunks across ADU boundaries
            c.s.sendall(data[i:i + 5])
            time.sleep(0.01)
        r = [c.recv_adu() for _ in range(3)]
        self.assertEqual([x[0] for x in r], [100, 101, 102])     # order kept
        self.assertEqual(r[0][2][2:], struct.pack(">2H", *self.sim.hr[1:3]))
        self.assertEqual(r[1][2], bytes([0x83, 2]))
        self.assertEqual(r[2][2][2:], struct.pack(">H", self.sim.hr[10]))

    def test_more_pipelined_requests_than_queue_slots(self):
        c = self.client()
        c.s.sendall(b"".join(c.adu(struct.pack(">BHH", 3, i, 1), tid=i) for i in range(30)))
        for i in range(30):
            tid, _, pdu = c.recv_adu()
            self.assertEqual(tid, i)
            self.assertEqual(struct.unpack(">H", pdu[2:])[0], self.sim.hr[i])

    def test_concurrent_clients_never_overlap_on_the_bus(self):
        errors = []

        def worker(k):
            try:
                c = Client(self.port)
                for i in range(10):
                    a = (k * 20 + i) % 240
                    if c.read_hr(a, 5) != self.sim.hr[a:a + 5]:
                        errors.append("wrong data %d" % a)
                c.close()
            except Exception as e:  # noqa: BLE001
                errors.append(repr(e))

        threads = [threading.Thread(target=worker, args=(k,)) for k in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(30)
        self.assertEqual(errors, [])
        req = [t for t, _ in self.sim.requests]
        self.assertEqual(len(req), 40)
        # every request arrives after the previous answer plus the configured gap (20 ms)
        for prev_answer, nxt in zip(self.sim.answered, req[1:]):
            self.assertGreaterEqual(nxt - prev_answer, 0.015)

    def test_line_noise_before_request_is_flushed(self):
        self.sim.inject(b"\x55\xaa\x00\x20\x03")
        time.sleep(0.2)
        self.assertEqual(self.client().read_hr(5, 1), self.sim.hr[5:6])
        self.assertEqual(len(self.sim.frames()), 1)


class FaultTests(GatewayTest):
    def test_lost_answer_is_retried(self):
        self.sim.drop = 1
        self.assertEqual(self.client().read_hr(1, 4), self.sim.hr[1:5])
        self.assertEqual(len(self.sim.frames()), 2)

    def test_bad_crc_is_retried(self):
        self.sim.corrupt = 1
        self.assertEqual(self.client().read_hr(1, 4), self.sim.hr[1:5])
        self.assertEqual(len(self.sim.frames()), 2)
        self.assertIn("bad crc", self.logtext())

    def test_no_answer_gives_gateway_exception_not_zeros(self):
        self.sim.drop = 99
        t = time.monotonic()
        with self.assertRaises(ModbusError) as e:
            self.client().read_hr(1, 4)
        self.assertEqual(e.exception.code, 0x0B)
        self.assertEqual(len(self.sim.frames()), 2)         # 1 attempt + 1 retry
        self.assertLess(time.monotonic() - t, 1.5)
        self.sim.drop = 0
        self.assertEqual(self.client().read_hr(1, 4), self.sim.hr[1:5])

    def test_client_disconnect_while_request_in_flight(self):
        self.sim.delay = (0.2, 0.2)
        c = Client(self.port)
        c.s.sendall(c.adu(struct.pack(">BHH", 3, 1, 1)))
        time.sleep(0.05)
        c.close()
        time.sleep(0.3)
        self.assertEqual(self.client().read_hr(2, 1), self.sim.hr[2:3])

    def test_invalid_header_closes_only_that_connection(self):
        good, bad = self.client(), self.client()
        bad.s.sendall(struct.pack(">HHHB", 1, 1, 6, 1) + struct.pack(">BHH", 3, 1, 1))
        self.assertEqual(bad.s.recv(10), b"")
        self.assertEqual(good.read_hr(3, 1), self.sim.hr[3:4])

    def test_oldest_client_is_evicted_when_full(self):
        self.proc.terminate()
        self.proc.wait(5)
        self.start_gateway(["-m", "2"])
        c1 = self.client()
        c1.read_hr(1, 1)
        time.sleep(0.05)
        c2 = self.client()
        c2.read_hr(1, 1)
        c3 = self.client()
        self.assertEqual(c3.read_hr(1, 1), self.sim.hr[1:2])
        self.assertEqual(c1.s.recv(10), b"")
        self.assertEqual(c2.read_hr(2, 1), self.sim.hr[2:3])


class WriteDisabledTests(GatewayTest):
    def test_writes_are_refused_by_default(self):
        c = self.client()
        for call in (lambda: c.write_hr(38, 485), lambda: c.write_hrs(38, [485]), lambda: c.write_coil(10, True)):
            with self.assertRaises(ModbusError) as e:
                call()
            self.assertEqual(e.exception.code, 1)
        self.assertEqual(self.sim.frames(), [])
        self.assertIn("rejected fc6 a=38 v=485 ex=1", self.logtext())


class WriteTests(GatewayTest):
    ARGS = ["-w", "38,92,94,158", "-c", "10", "-z", "158-161"]

    def test_allowed_writes_reach_the_slave(self):
        c = self.client()
        self.assertEqual(c.write_hr(38, 485), struct.pack(">BHH", 6, 38, 485))
        self.assertEqual(self.sim.hr[38], 485)
        c.write_hrs(94, [235])
        self.assertEqual(self.sim.hr[94], 235)
        c.write_coil(10, True)
        self.assertTrue(self.sim.coils[10])
        self.assertEqual(c.read_hr(38, 1), [485])
        self.assertIn("fc6 a=38 v=485 ok", self.logtext())

    def test_writes_outside_allowlist_are_refused(self):
        c = self.client()
        for call in (lambda: c.write_hr(39, 1), lambda: c.write_hrs(92, [1, 2]), lambda: c.write_coil(11, True)):
            with self.assertRaises(ModbusError) as e:
                call()
            self.assertEqual(e.exception.code, 2)
        self.assertEqual(self.sim.frames(), [])

    def test_zero_is_never_written_to_the_nonzero_list(self):
        c = self.client()
        with self.assertRaises(ModbusError) as e:
            c.write_hr(158, 0)
        self.assertEqual(e.exception.code, 3)
        with self.assertRaises(ModbusError):
            c.write_hrs(158, [0])
        self.assertEqual(self.sim.frames(), [])
        c.write_hr(158, 1)
        self.assertEqual(self.sim.hr[158], 1)

    def test_lost_write_answer_is_retried(self):
        self.sim.drop = 1
        self.client().write_hr(38, 480)
        self.assertEqual(self.sim.hr[38], 480)
        self.assertEqual(len(self.sim.frames()), 2)


class RangeTests(GatewayTest):
    ARGS = ["-w", "38,159", "-z", "158", "-V", "38=350..600,159=0..1"]

    def test_values_outside_the_range_are_refused(self):
        c = self.client()
        for call in (lambda: c.write_hr(38, 700), lambda: c.write_hr(38, 349), lambda: c.write_hr(159, 2),
                     lambda: c.write_hrs(38, [700])):
            with self.assertRaises(ModbusError) as e:
                call()
            self.assertEqual(e.exception.code, 3)
        self.assertEqual(self.sim.frames(), [])
        self.assertIn("ranges=38=350..600,159=0..1", self.logtext())

    def test_values_inside_the_range_are_written(self):
        c = self.client()
        c.write_hr(38, 600)
        c.write_hr(159, 0)
        self.assertEqual((self.sim.hr[38], self.sim.hr[159]), (600, 0))
        c.write_hr(159, 1)
        self.assertEqual(self.sim.hr[159], 1)


class RateLimitTests(GatewayTest):
    ARGS = ["-w", "38", "-R", "1"]

    def test_second_write_within_limit_is_busy(self):
        c = self.client()
        c.write_hr(38, 480)
        with self.assertRaises(ModbusError) as e:
            c.write_hr(38, 485)
        self.assertEqual(e.exception.code, 6)
        self.assertEqual(self.sim.hr[38], 480)
        time.sleep(1.1)
        c.write_hr(38, 485)
        self.assertEqual(self.sim.hr[38], 485)


class RealisticTimingTests(GatewayTest):
    """slow answers like pcod measured on the reference controller (0.27-0.42 s); default timeouts"""
    SIM = {"delay": (0.27, 0.42)}
    ARGS = ["-T", "1500", "-g", "100"]

    def test_slow_slave_without_errors(self):
        c = self.client()
        for i in range(8):
            self.assertEqual(c.read_hr(1 + i * 20, 20), self.sim.hr[1 + i * 20:21 + i * 20])
        self.assertNotIn("timeout fc", self.logtext())
        self.assertNotIn("attempt", self.logtext())


class LifecycleTests(GatewayTest):
    ARGS = ["-t", "2"]

    def test_watchdog_stats_and_duration(self):
        self.expect_exit = True
        self.assertEqual(read(self.wdog), "pwdog\n")     # fed right at start
        os.unlink(self.wdog)
        c = self.client()
        c.read_hr(1, 1)
        with self.assertRaises(ModbusError):
            c.read_hr(300, 1)
        time.sleep(0.1)
        c.close()
        self.clients.remove(c)
        self.assertEqual(self.proc.wait(4), 0)
        self.assertEqual(read(self.wdog), "pwdog\n")     # fed again after ~2 s
        s = self.stats()
        self.assertEqual((s["requests"], s["ok"], s["rejected"], s["failed"]), ("2", "1", "1", "0"))
        self.assertIn("mbproxy exit", self.logtext())


class DefaultsTests(GatewayTest):
    """without options: slave 1, no local address limits, nothing protected from 0 (writes still need -w)"""
    BASE = []
    SIM = {"slave": 1}
    ARGS = ["-w", "158"]

    def test_slave_1_and_addresses_left_to_the_controller(self):
        c = self.client()
        self.assertEqual(c.read_hr(1, 2), self.sim.hr[1:3])
        self.assertEqual(self.sim.frames()[0][0], 1)
        with self.assertRaises(ModbusError) as e:
            c.read_hr(240, 20)                      # refused by the controller, not by mbproxy
        self.assertEqual(e.exception.code, 2)
        self.assertEqual(len(self.sim.frames()), 2)
        c.write_hr(158, 0)
        self.assertEqual(self.sim.hr[158], 0)


class BindTests(unittest.TestCase):
    """port 502 is still held by tcpmodbus' TIME_WAIT connections right after the takeover"""

    def setUp(self):
        self.sim = SlaveSim()
        self.port = free_port()
        self.blocker = socket.socket()
        self.blocker.bind(("0.0.0.0", self.port))
        self.blocker.listen(1)

    def tearDown(self):
        self.blocker.close()
        self.sim.close()

    def run_gateway(self, *args):
        return subprocess.Popen([BINARY, "-d", self.sim.path, "-p", str(self.port), "-W", "none", "-s", "32", *args],
                                stderr=subprocess.PIPE)

    def test_waits_for_busy_port(self):
        p = self.run_gateway("-B", "10")
        try:
            time.sleep(1.5)
            self.assertIsNone(p.poll())
            self.blocker.close()
            for _ in range(50):
                try:
                    c = Client(self.port)
                    break
                except OSError:
                    time.sleep(0.1)
            else:
                self.fail("gateway did not take the port")
            self.assertEqual(c.read_hr(1, 2), self.sim.hr[1:3])
            c.close()
        finally:
            p.terminate()
            err = p.communicate(timeout=5)[1]
        self.assertIn(b"port busy, retrying", err)
        self.assertIn(b"listening", err)

    def test_gives_up_after_bind_wait(self):
        p = self.run_gateway("-B", "1")
        self.assertEqual(p.wait(5), 1)
        self.assertIn(b"cannot listen on port", p.communicate()[1])


class CliTests(unittest.TestCase):
    def test_bad_options(self):
        for args in (["-x"], ["-p"], ["-p", "0"], ["-w", "1;2"], ["-m", "9"], ["-P", "x"], ["-V", "38=5..1"]):
            r = subprocess.run([BINARY] + args, capture_output=True)
            self.assertEqual(r.returncode, 2, args)
            self.assertIn(b"usage", r.stderr)

    def test_missing_serial_device(self):
        r = subprocess.run([BINARY, "-d", "/nonexistent", "-W", "none"], capture_output=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"cannot open serial device", r.stderr)

    def test_query_does_not_need_a_listener(self):
        sim = SlaveSim()
        try:
            r = subprocess.run([BINARY, "-q", "-d", sim.path], capture_output=True)
            self.assertEqual(r.returncode, 0)
            self.assertIn(b"cflag=", r.stdout)
        finally:
            sim.close()


if __name__ == "__main__":
    BINARY = os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
