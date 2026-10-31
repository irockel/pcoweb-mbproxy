"""Mark an ELF as old-ABI ARM like the native pCOWeb binaries.

usage: fixelf.py OUT            set EI_OSABI = ARM (97) and e_flags = 0
       fixelf.py REF OUT        copy EI_OSABI and e_flags from REF (e.g. pcod fetched from the card)
"""
import struct, sys
if len(sys.argv) == 3:
    ref = open(sys.argv[1], "rb").read(64)
    osabi, flags = ref[7], struct.unpack("<I", ref[36:40])[0]
    path = sys.argv[2]
else:
    osabi, flags, path = 97, 0, sys.argv[1]
out = bytearray(open(path, "rb").read())
out[7] = osabi
out[36:40] = struct.pack("<I", flags)
open(path, "wb").write(out)
print("EI_OSABI", out[7], "e_flags", hex(flags))
