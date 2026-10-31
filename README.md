# pcoweb-mbproxy

A drop-in replacement for `pcod` + `tcpmodbus` on the **Carel pCOWeb** card: a small, transparent
Modbus TCP → Modbus RTU gateway that runs on the card itself. Each Modbus TCP request goes straight to the pCO
controller and its answer comes straight back. No cache, no background polling, no values that drop to zero,
and writes that work on the first attempt.

Many heat pumps, air handlers, chillers and refrigeration units run on a Carel pCO controller. The pCOWeb is the
Ethernet card for its BMS slot, and plenty of them are sold second-hand. If the controller speaks **Modbus RTU
slave** on its BMS port, mbproxy can take over the card's serial line.

> **Status:** has run in production since October 2026 on one pCOWeb (ARM720T, firmware 1.4.10) in a
> Waterkotte heat pump ([example](examples/waterkotte-wwpr/)). Other card revisions and firmware versions are
> untested. Reports are welcome.

## Why

With the stock software, `pcod` polls the controller continuously into a shared-memory cache. `tcpmodbus`
(port 502) only reads that cache and hands writes to `pcod` through a message queue. The results:

- **values flap to 0** every few seconds: a slow or missed answer invalidates a cached block, and `tcpmodbus`
  serves it as zeros (the web UI shows "U")
- **writes are acknowledged but not applied**, or only after several attempts: the write races with the next
  poll of the same block
- the poll cycle limits how quickly changes show up

mbproxy instead:

- forwards every request 1:1 and returns the controller's answer, with one FIFO for all clients and one
  transaction on the line at a time
- answers with exception **0x0B** (gateway target failed to respond) instead of zeros when the controller does
  not answer. It retries once first.
- refuses all writes unless allowed explicitly: write allowlist (`-w`, `-c`), value ranges per register (`-V`),
  never-0 list (`-z`) and a rate limit (`-R`). These checks cost no bus traffic.
- feeds the card's hardware watchdog from its main loop. If mbproxy hangs, the card reboots into the stock
  setup.
- needs ~15 KB of flash and keeps logs and statistics in RAM only

Measured on the reference controller: answers in 20–240 ms (avg 107 ms) with mbproxy; `pcod` had seemed to need
0.27–0.42 s. In 10 minutes of back-to-back reads there were 0 timeouts and 0 all-zero blocks.

## Requirements

- pCOWeb card with telnet and FTP access (Linux 2.4 on ARM, the classic hardware). See
  [docs/pcoweb-card.md](docs/pcoweb-card.md) for how to get a used card running.
- pCO controller set to **Modbus slave** on the BMS port the card sits in. You need its slave address, baud rate
  and parity.
- A build machine with `arm-none-eabi-gcc` (e.g. Homebrew `arm-none-eabi-gcc`, Debian/Ubuntu
  `gcc-arm-none-eabi`), `make`, a host C compiler and Python 3. No libc for ARM is needed.
  Without a toolchain: download `mbproxy` from the
  [releases](https://github.com/irockel/pcoweb-mbproxy/releases), check it against `SHA256SUMS` and put it at
  `src/build/mbproxy`, where the tools expect it.

## Quick start

```bash
cd src && make && make test       # build/mbproxy (card) + build/mbproxy-host, unit + end-to-end tests
export PCOWEB_HOST=192.0.2.10 PCOWEB_USER=... PCOWEB_PASS=...   # the card's telnet/FTP login
```

1. **Trial run, read-only.** `pcod` is stopped for the duration and nothing is written to flash:

   ```bash
   tools/trial.py -t 300 -r 3:1:100 -- -s 1      # -s = slave address of your controller
   ```

   It compares mbproxy's answers with `pcod`'s cache and prints latency and errors.

2. **Configure.** Copy [card/mbproxy/mbproxy.conf](card/mbproxy/mbproxy.conf) and set at least the slave
   address. Leave writes off until reading works.

3. **Install** to the card's flash. This starts nothing; it takes effect at the next boot:

   ```bash
   tools/install.py my-mbproxy.conf
   ```

4. **Reboot the card.** `pcod` starts as usual. **60 s later** the supervisor stops `pcod` and `tcpmodbus` and
   starts mbproxy on port 502. mbproxy waits until the old connections have left TIME_WAIT (~60 s). Clients
   reconnect on their own.

Check it via telnet:

```bash
/usr/flashdisk/etc/rc_users/S50mbproxy status   # processes, supervisor log, statistics
cat /tmp/mbproxy.log
```

## Safety nets

The supervisor [card/rc_users/S50mbproxy](card/rc_users/S50mbproxy) is started by the card's own plugin hook
(`S99userscript`):

| Situation | What happens |
|---|---|
| no `mbproxy.conf` on the card | stays with `pcod` |
| **kill switch** `touch /usr/flashdisk/mbproxy/disable` | stays with `pcod` at the next start |
| back to `pcod` at runtime | `S50mbproxy stop` (`start` takes over again) |
| mbproxy exits / crashes | restarted after 1 s; after 3 exits within 10 min each → back to `pcod` |
| mbproxy hangs | the watchdog is no longer fed → card reboots after ~10 s → `pcod`, 60 s later mbproxy again |
| boot loop | a boot counts as failed until mbproxy has run 10 min; after 3 failed boots in a row the card stays with `pcod` (`rm /usr/flashdisk/mbproxy/bootcount` to retry) |
| uninstall | `rm /usr/flashdisk/etc/rc_users/S50mbproxy`, reboot |

After every boot there are 60 s with `pcod` (`GRACE`), so telnet/FTP always get a window to set the kill switch.

## Options

`mbproxy -h`:

```
  -p port      Modbus TCP port (502)
  -d device    serial device (/dev/ttyAM0)
  -b baud      9600 | 19200 | 38400 (19200)
  -P parity    n | e | o (n)
  -s slave     Modbus RTU slave address of the pCO (1)
  -T ms        response timeout per attempt (1500)
  -r n         retries after timeout / bad frame (1)
  -g ms        silence between transactions (100)
  -a addr      highest holding register; requests beyond it are refused without bus
               traffic (65535 = no check)
  -k addr      highest coil, as -a (65535)
  -w list      holding registers that may be written, e.g. 38,92,94 (none)
  -c list      coils that may be written (none)
  -z list      holding registers never written with 0 (none)
  -V ranges    allowed raw values per holding register, e.g. 38=350..600,159=0..1
               (signed; registers without a range accept any value)
  -R s         minimum seconds between writes to one address (0 = off)
  -W path      watchdog file, 'none' to disable (card: /proc/hwio)
  -l path      log file (stderr)
  -L bytes     truncate the log file beyond this size (65536)
  -S path      statistics file, rewritten every 10 s
  -t s         exit after this many seconds (0 = run forever)
  -B s         keep trying to bind the port this long, e.g. while tcpmodbus' connections
               are in TIME_WAIT (120)
  -m n         maximum TCP clients, 1-8 (4)
  -q           print the current settings of the serial device and exit
  -v           log every request and frame
```

Supported function codes: 1–6, 15, 16, within the Modbus limits per request (read 125 registers / 2000 coils,
write 123 / 1968). Any unit id is
accepted; requests always go to the slave given with `-s`. Write allowlists cover addresses 0–1023.

Run `mbproxy -q` while `pcod` is running to see the serial settings `pcod` uses.

## How it is built

The card runs Linux 2.4.21 on an ARM720T (ARMv4T) with the **old ARM ABI** (OABI) and glibc 2.2. Instead of an
old cross toolchain, mbproxy is built **freestanding** with a current `arm-none-eabi-gcc`:

- no libc: system calls are made directly with `swi 0x900000 + nr`, networking through `socketcall`
  ([src/plat_pcoweb.c](src/plat_pcoweb.c)). Only libgcc is linked, for division.
- the ELF header is patched to OABI ([src/fixelf.py](src/fixelf.py)). The kernel structures used have no 64-bit
  members, so their layout is the same in both ABIs.
- `times()` serves as monotonic clock (HZ = 100); the card has no RTC and its wall clock starts at 1970
- the same code runs on the host with [src/plat_posix.c](src/plat_posix.c) for the tests:
  [src/test/test_core.c](src/test/test_core.c) (framing, CRC, validation) and
  [src/test/test_gateway.py](src/test/test_gateway.py) (end-to-end against a simulated pCO on a pseudo
  terminal, with dropped answers, broken CRCs and slow answers)

`make hello` builds a minimal "hello world" for the card, to check the toolchain and the upload first.

## Security

The pCOWeb has no protection of its own: HTTP, FTP and Telnet use well-known Carel default credentials
([list](docs/pcoweb-card.md#default-users-and-passwords)), and
Modbus TCP has no authentication at all. Change the passwords and keep the card in a network segment that only
your home automation can reach. mbproxy's write allowlist limits what a client can change on the controller; it
does not replace network isolation.

## Disclaimer

This replaces vendor software on a device that controls real equipment. Writing wrong values can switch units
off or damage them. Writing parameters often wears the controller's non-volatile memory. Use it at your own
risk, start read-only, and only allow writes to registers whose meaning you have verified on your controller.
This project is not affiliated with Carel or any equipment manufacturer.

## License

[Apache License 2.0](LICENSE)
