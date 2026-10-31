# Getting a used pCOWeb card running

Notes from bringing a second-hand pCOWeb back to life on a pCO controller that was not the one it came from. Most
of this applies whether or not you use mbproxy.

## The card

| | |
|---|---|
| Hardware | ARM720T (ARMv4T, Cirrus CLPS711x), ~15 MB RAM (≈ 0.3 MB free while running) |
| OS | Linux 2.4.21, old ARM ABI (OABI), glibc 2.2.3, `bash` |
| File systems | root and `/usr/local` are cramfs (read-only); `/usr/flashdisk` is JFFS2 (persistent, writable) |
| Services | HTTP 80 (`thttpd`), FTP 21, Telnet 23, Modbus TCP 502 (`tcpmodbus`), SNMP |
| Serial | `/dev/ttyAM0` to the pCO's BMS port, owned by `pcod` |
| Clock | no RTC: the clock starts at 1970 after every boot |

Web admin, FTP and Telnet use the **Carel default credentials** from the pCOWeb manual. Change them.

## Firmware

The reference card runs firmware **1.4.10**, the last 1.4.x release, installed following the Carel knowledge
base. Carel describes a **new Modbus layout** for 1.5.x. If your controller and your clients rely on the 1.4.x
register mapping, do not update to 1.5.x. mbproxy itself has not been tried on 1.5.x: the plugin hook, the
watchdog interface and the start scripts it relies on may differ there.

<!-- TODO: describe the firmware update procedure (where to get 1.4.10, how it is flashed) -->

## Controller and card must agree

A factory reset of the card sets the **Carel** protocol. If the controller's BMS port is set to **Modbus slave**,
nothing works, and the web UI shows **"U"** (undefined) for every variable.

1. On the controller: BMS menu → protocol **Modbus slave**, note the slave address. Some controllers only keep
   changes in this menu when a "save" option is switched on.
2. On the card: Configuration → pCO Com (or `/usr/flashdisk/etc/sysconfig/commcfg`) → the same protocol,
   address, baud rate and parity. Back up the original file first (e.g. `commcfg.carel`).
3. Link check via telnet: `cat /proc/tty/driver/*` shows the serial counters. If `rx` stays at 0 on `ttyAM0`
   while `tx` grows, the controller does not answer. Then the protocol, address or serial settings are wrong.
   Sweeping baud rates (9600/19200/38400) and parity is quick.

## pcod's address limits

`pcod` polls a fixed address range: by default 207 digitals and 5000 analog/integer variables, more than many
controllers have. Requests beyond the controller's range are answered with exception 02/01, and the whole block
stays "U". Only the **Modbus Extended** protocol (`proto=30` in `commcfg`) honours the limits `max_d`, `max_a`
and `max_i`, so set those to what your controller actually has. In this mode the integers follow the analogs
(e.g. `In` = HR 128 + n on the reference controller), so `max_i=0` if there is no separate integer range.
See [the example](../examples/waterkotte-wwpr/commcfg).

With mbproxy these limits no longer matter: `pcod` is not running, and clients read exactly what they ask for.
`mbproxy -a/-k` can refuse reads beyond the controller's range locally.

## Things that bite

- **Watchdog:** the kernel watchdog `WDOG_PCO` (`cat /proc/hwio`, counts down from 1000 ≈ 10 s) reboots the card
  unless `pwdog` is written to `/proc/hwio` regularly. `pcod` does that. Stop `pcod` without feeding the watchdog
  and the card reboots ~10 s later. Feeding it by hand: `while :; do echo pwdog > /proc/hwio; sleep 2; done`.
- **pcod dies with your telnet session:** a `pcod` started from telnet gets SIGHUP when the session ends, and
  ~10 s later the watchdog reboots the card. Start it detached:
  `(/etc/rc.d/rc2.d/S91pcod start < /dev/null > /dev/null 2>&1 &)`.
- **Port 502 stays blocked for 60 s:** `tcpmodbus` does not set `SO_REUSEADDR`. After it is killed, nobody can
  bind port 502 while its connections are in TIME_WAIT. mbproxy keeps retrying (`-B`).
- **Old bash:** it returns from a *stopped* child as if it had exited. A supervisor loop then starts a second
  copy while the first still holds the port and the serial line.
- **Plugin hook:** `S99userscript` runs every executable in `/usr/flashdisk/etc/rc_users/` at boot, after
  `S91pcod` and `S98modbustcp`, if plugins are enabled. This is how mbproxy starts without touching the
  read-only root file system.

## Useful on the card

```bash
cat /proc/tty/driver/*                  # serial tx/rx counters
cat /proc/hwio                          # watchdog counters
pidof pcod tcpmodbus
cat /usr/flashdisk/etc/sysconfig/commcfg
```

`/config/xml.cgi` (HTTP POST, body `D|1|n|I|1|n|A|1|n`) returns `pcod`'s view of all variables.

## Serial settings pcod uses

`mbproxy -q` while `pcod` runs prints them. On the reference card: iflag = IGNBRK, oflag = lflag = 0,
cflag = B19200 | CS8 | CREAD | CLOCAL, VMIN = 1, VTIME = 0. mbproxy sets the same, except VMIN = 0 because it
uses `select()`.
