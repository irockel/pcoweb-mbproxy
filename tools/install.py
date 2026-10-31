#!/usr/bin/env python3
"""Install mbproxy as pCOWeb plugin: copies to the card's flash (JFFS2)

  src/build/mbproxy              -> /usr/flashdisk/mbproxy/mbproxy
  CONF (your mbproxy.conf)       -> /usr/flashdisk/mbproxy/mbproxy.conf
  card/rc_users/S50mbproxy       -> /usr/flashdisk/etc/rc_users/S50mbproxy   (started by S99userscript at boot)

Files are uploaded under a temporary name and renamed, so a running mbproxy can be replaced (it keeps running
the old binary until it is restarted). Does not start or stop anything; takes effect at the next boot or with
  /usr/flashdisk/etc/rc_users/S50mbproxy stop; /usr/flashdisk/etc/rc_users/S50mbproxy start

usage: install.py CONF        (card and credentials: see tools/pcoweb.py)
"""
import ftplib, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcoweb import ftp, tn  # noqa: E402

if len(sys.argv) != 2:
    raise SystemExit(__doc__)
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FILES = [
    (os.path.join(R, "src", "build", "mbproxy"), "/usr/flashdisk/mbproxy/mbproxy"),
    (sys.argv[1], "/usr/flashdisk/mbproxy/mbproxy.conf"),
    (os.path.join(R, "card", "rc_users", "S50mbproxy"), "/usr/flashdisk/etc/rc_users/S50mbproxy"),
]
for src, _ in FILES:
    if not os.path.isfile(src):
        raise SystemExit("missing %s (run make in src/ first)" % src)

f = ftp()
for d in ("/usr/flashdisk/mbproxy", "/usr/flashdisk/etc/rc_users"):
    try:
        f.mkd(d)
    except ftplib.error_perm:
        pass                                    # exists
for src, dst in FILES:
    with open(src, "rb") as b:
        f.storbinary("STOR %s.new" % dst, b)
    f.rename(dst + ".new", dst)
    print("installed", dst, os.path.getsize(src), "bytes")
f.quit()
print(tn("chmod 755 /usr/flashdisk/mbproxy/mbproxy /usr/flashdisk/etc/rc_users/S50mbproxy",
         "ls -la /usr/flashdisk/mbproxy /usr/flashdisk/etc/rc_users").split("\n", 4)[-1])
