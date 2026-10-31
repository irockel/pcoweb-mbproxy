#!/usr/bin/env python3
"""Read-only trial run of src/build/mbproxy on the pCOWeb card, before installing it.

1. read the blocks through pcod (port 502) as reference
2. upload mbproxy + a run script to /tmp (RAM) and start it: stop pcod, mbproxy -p 5020 -t DURATION, start pcod
3. read the blocks through mbproxy on port 5020 back to back until shortly before the end
4. fetch log and statistics, check that pcod runs again, print a summary and the differences to pcod

Writes nothing to the controller (no write allowlist). STOPS PCOD FOR DURATION SECONDS: whatever else reads
the card (web UI, SNMP, your home automation) gets nothing meanwhile. If anything goes wrong, a reboot of the
card brings back the stock setup - nothing is changed on the flash.

usage: trial.py [-t DURATION_S] [-r FC:ADDR:N ...] [-- MBPROXY_ARGS]
  -t   duration in seconds (default 600)
  -r   block to read, e.g. 3:1:100 (FC 3 holding registers, 1 coils); repeatable, default 3:1:50
  MBPROXY_ARGS  e.g. -s 32 -a 249 -k 199 (slave address and limits of your controller)
example: trial.py -t 300 -r 3:1:125 -r 1:0:100 -- -s 32
"""
import ftplib, io, os, socket, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcoweb import HOST, Conn, decode, fetch, ftp, tn  # noqa: E402

BINARY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "build", "mbproxy")
PORT = 5020


def run_script(duration, args):
    """on the card: stop pcod, run mbproxy (until -t expires or it is killed), start pcod again detached"""
    return f"""kill $(pidof pcod)
sleep 1
/tmp/mbproxy -p {PORT} -t {duration} -l /tmp/mbproxy.log -S /tmp/mbproxy.stat {args}
echo "mbproxy_rc=$?" > /tmp/trial.done
(/etc/rc.d/rc2.d/S91pcod start < /dev/null > /dev/null 2>&1 &)
sleep 3
echo "pcod=$(pidof pcod)" >> /tmp/trial.done
"""


def parse(argv):
    duration, blocks, extra = 600, [], []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    it = iter(argv)
    for a in it:
        if a == "-t":
            duration = int(next(it))
        elif a == "-r":
            fc, addr, n = (int(x) for x in next(it).split(":"))
            blocks.append((fc, addr, n))
        else:
            raise SystemExit(__doc__)
    return duration, blocks or [(3, 1, 50)], " ".join(extra)


def read_all(conn, blocks):
    out = {}
    for fc, addr, n in blocks:
        data, exc = conn.call(fc, addr, n)
        out[(fc, addr, n)] = decode(fc, data, n) if data is not None else "exception %d" % exc
    return out


def snapshot_pcod(blocks):
    for _ in range(10):
        try:
            c = Conn(502)
            r = read_all(c, blocks)
            c.close()
            return r
        except OSError:
            time.sleep(1)
    raise SystemExit("no answer from pcod on port 502")


def start_mbproxy(duration, args):
    f = ftp()
    for p in ("/tmp/trial.done", "/tmp/mbproxy.log", "/tmp/mbproxy.stat"):
        try:
            f.delete(p)
        except ftplib.error_perm:
            pass
    with open(BINARY, "rb") as b:
        f.storbinary("STOR /tmp/mbproxy", b)
    f.storbinary("STOR /tmp/trial.sh", io.BytesIO(run_script(duration, args).encode()))
    f.quit()
    tn("chmod 755 /tmp/mbproxy", "(bash /tmp/trial.sh < /dev/null > /dev/null 2>&1 &)")
    t0 = time.monotonic()
    print("started, waiting for port %d ..." % PORT)
    for _ in range(30):
        try:
            socket.create_connection((HOST, PORT), timeout=2).close()
            return t0
        except OSError:
            time.sleep(1)
    print(tn("cat /tmp/mbproxy.log /tmp/trial.done; pidof pcod"))
    raise SystemExit("mbproxy does not listen")


def wait_finished(t0, duration):
    print("waiting for mbproxy to exit and pcod to start ...")
    while True:
        time.sleep(5)
        try:
            f = ftp()
            done = fetch(f, "/tmp/trial.done")
            if done and "pcod=" in done:
                log, stat = fetch(f, "/tmp/mbproxy.log"), fetch(f, "/tmp/mbproxy.stat")
                f.quit()
                return done, log, stat
            f.quit()
        except OSError:
            pass
        if time.monotonic() - t0 > duration + 120:
            raise SystemExit("run script did not finish - reboot the card if pcod does not come back")


def main():
    duration, blocks, args = parse(sys.argv[1:])
    if not os.path.isfile(BINARY):
        raise SystemExit("missing %s (run make in src/ first)" % BINARY)
    print("reference via pcod (port 502) ...")
    ref = snapshot_pcod(blocks)

    t0 = start_mbproxy(duration, args)
    first, lat, exc, errors = None, [], {}, []
    try:
        c = Conn(PORT)
        while time.monotonic() < t0 + duration - 20:
            for fc, addr, n in blocks:
                t = time.monotonic()
                data, e = c.call(fc, addr, n)
                if e is None:
                    lat.append(time.monotonic() - t)
                else:
                    key = "fc%d a=%d n=%d ex=%d" % (fc, addr, n, e)
                    exc[key] = exc.get(key, 0) + 1
            if first is None:
                first = read_all(c, blocks)
            if len(lat) % 50 < len(blocks):
                print("  %3.0f s  ok=%d exceptions=%d" % (time.monotonic() - t0, len(lat), sum(exc.values())),
                      flush=True)
        c.close()
    except OSError as e:
        errors.append(repr(e))

    done, log, stat = wait_finished(t0, duration)
    print("\n=========== summary")
    print(done.strip())
    print("\n--- mbproxy stats\n" + (stat or "(none)"))
    print("--- mbproxy log (last 40 lines)\n" + "\n".join((log or "(none)").splitlines()[-40:]))
    if lat:
        lat.sort()
        print("client view: ok=%d  latency ms min %.0f avg %.0f p95 %.0f max %.0f" % (
            len(lat), lat[0] * 1e3, sum(lat) / len(lat) * 1e3, lat[int(len(lat) * .95)] * 1e3, lat[-1] * 1e3))
    print("exceptions:", exc or "none", " connection errors:", errors or "none")
    if first:
        for blk, values in first.items():
            r = ref[blk]
            if isinstance(r, str) or isinstance(values, str):
                print("fc%d %d+%d: pcod %s, mbproxy %s" % (*blk, r if isinstance(r, str) else "ok",
                                                            values if isinstance(values, str) else "ok"))
                continue
            diff = [(blk[1] + i, a, b) for i, (a, b) in enumerate(zip(r, values)) if a != b]
            print("fc%d %d+%d: %d of %d differ from pcod's cache%s" % (*blk, len(diff), blk[2],
                  " (slowly changing values are expected)" if diff else ""))
            for a, x, y in diff[:30]:
                print("  %5d: pcod %6d  mbproxy %6d" % (a, x, y))
    print(tn("rm -f /tmp/mbproxy /tmp/trial.sh; pidof pcod tcpmodbus").strip().splitlines()[-1:])


if __name__ == "__main__":
    main()
