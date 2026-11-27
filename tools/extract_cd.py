#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract the data track of the GOG CD image (game.gog, raw MODE2/2352) to a directory.

    tools/extract_cd.py [game-data-dir] [--out extracted/cd]

The runner maps drive D: to that directory (tools/run.sh passes --cd-dir).
"""
import argparse
import os
import struct
import sys

SECTOR = 2352
USER_OFF = 24  # MODE2 form 1: 12 sync + 4 header + 8 subheader


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game_dir", nargs="?",
                    default=os.environ.get("FG_GAME_DIR",
                                           os.path.expanduser("~/GOG Games/Fantasy General/game/data")))
    ap.add_argument("--out", default="extracted/cd")
    a = ap.parse_args()

    img = os.path.join(a.game_dir, "game.gog")
    f = open(img, "rb")

    def sectors(lba, count):
        out = bytearray()
        for i in range(count):
            f.seek((lba + i) * SECTOR + USER_OFF)
            out += f.read(2048)
        return bytes(out)

    pvd = sectors(16, 1)
    if pvd[1:6] != b"CD001":
        sys.exit("not an ISO 9660 data track: " + img)

    count = 0

    def walk(lba, size, path):
        nonlocal count
        data = sectors(lba, (size + 2047) // 2048)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            elba, esize = struct.unpack_from("<I", data, i + 2)[0], struct.unpack_from("<I", data, i + 10)[0]
            flags = data[i + 25]
            name = data[i + 33:i + 33 + data[i + 32]]
            if name not in (b"\0", b"\1"):
                name = name.decode("ascii").split(";")[0]
                dest = os.path.join(path, name)
                if flags & 2:
                    os.makedirs(dest, exist_ok=True)
                    walk(elba, esize, dest)
                else:
                    with open(dest, "wb") as out:
                        remaining, cur = esize, elba
                        while remaining > 0:
                            chunk = sectors(cur, min(64, (remaining + 2047) // 2048))
                            out.write(chunk[:remaining])
                            remaining -= len(chunk)
                            cur += 64
                    count += 1
            i += n

    os.makedirs(a.out, exist_ok=True)
    root = pvd[156:156 + 34]
    walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], a.out)
    print("extracted %d files to %s" % (count, a.out))


if __name__ == "__main__":
    main()
