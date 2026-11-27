#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract the 32-bit LE image from a DOS/4GW-bound executable (BARENA.EXE).

Outputs (into --out):
  info.json      header fields, object table, entry point
  fixups.json    every fixup record, decoded
  objN.bin       each object's memory image with fixups applied at its preferred base
  image.elf      ELF32 i386 file containing the objects at their preferred bases,
                 ready for Ghidra (or objdump) to import directly

The LE format reference is the IBM "LX - Linear eXecutable Module Format" spec;
LE (the Windows VxD / DOS4GW variant) differs mainly in the object page table entry.
"""
import argparse
import json
import os
import struct
import sys

# Fixup source types (low nibble of the source byte)
SRC_TYPES = {
    0x0: "byte",
    0x2: "sel16",
    0x3: "ptr16:16",
    0x5: "off16",
    0x6: "ptr16:32",
    0x7: "off32",
    0x8: "rel32",
}
SRC_SIZES = {0x0: 1, 0x2: 2, 0x3: 4, 0x5: 2, 0x6: 6, 0x7: 4, 0x8: 4}


class LEImage:
    def __init__(self, data):
        self.data = data
        self.mz_off, self.le_off = self._find_le()
        self._parse_header()
        self._parse_objects()
        self._parse_page_map()
        self._parse_fixups()

    # -- locating the LE header -------------------------------------------
    def _find_le(self):
        d = self.data
        # Walk the chain of MZ executables (DOS/4GW stub, then the bound program).
        off = 0
        while off + 0x40 <= len(d) and d[off:off + 2] == b"MZ":
            lfanew = struct.unpack_from("<I", d, off + 0x3C)[0]
            if 0 < lfanew < len(d) - off and d[off + lfanew:off + lfanew + 2] == b"LE":
                return off, off + lfanew
            last, pages = struct.unpack_from("<HH", d, off + 2)
            size = pages * 512 - ((512 - last) if last else 0)
            if size <= 0:
                break
            off += size
        # Fallback: scan for an MZ whose e_lfanew points at an LE signature.
        i = d.find(b"LE\x00\x00")
        while i != -1:
            for m in range(i - 0x40, -1, -1):
                if d[m:m + 2] == b"MZ" and m + struct.unpack_from("<I", d, m + 0x3C)[0] == i:
                    return m, i
            i = d.find(b"LE\x00\x00", i + 1)
        raise ValueError("no LE header found")

    def _u32(self, rel):
        return struct.unpack_from("<I", self.data, self.le_off + rel)[0]

    def _parse_header(self):
        h = {}
        h["cpu"], h["os"] = struct.unpack_from("<HH", self.data, self.le_off + 0x08)
        fields = {
            "module_flags": 0x10, "num_pages": 0x14, "eip_object": 0x18, "eip": 0x1C,
            "esp_object": 0x20, "esp": 0x24, "page_size": 0x28, "last_page_size": 0x2C,
            "fixup_section_size": 0x30, "loader_section_size": 0x38,
            "object_table_off": 0x40, "num_objects": 0x44, "object_page_table_off": 0x48,
            "resident_name_table_off": 0x58, "entry_table_off": 0x5C,
            "fixup_page_table_off": 0x68, "fixup_record_table_off": 0x6C,
            "import_module_table_off": 0x70, "num_import_modules": 0x74,
            "data_pages_off": 0x80, "num_preload_pages": 0x84, "auto_ds_object": 0x94,
            "debug_info_off": 0x98, "debug_info_len": 0x9C,
        }
        for k, v in fields.items():
            h[k] = self._u32(v)
        self.hdr = h

    def _parse_objects(self):
        h = self.hdr
        self.objects = []
        for i in range(h["num_objects"]):
            vsize, base, flags, pmi, pmc, _ = struct.unpack_from(
                "<IIIIII", self.data, self.le_off + h["object_table_off"] + 24 * i)
            self.objects.append({
                "index": i + 1, "virtual_size": vsize, "base": base, "flags": flags,
                "page_map_index": pmi, "page_count": pmc,
                "readable": bool(flags & 1), "writable": bool(flags & 2),
                "executable": bool(flags & 4), "big": bool(flags & 0x2000),
            })

    def _parse_page_map(self):
        h = self.hdr
        self.pages = []  # 1-based page index -> (file offset or None, size, type)
        data_base = self.mz_off + h["data_pages_off"]
        for p in range(h["num_pages"]):
            b = self.data[self.le_off + h["object_page_table_off"] + 4 * p:][:4]
            # LE entry: 3-byte page number (high byte first), 1 byte type.
            num = (b[0] << 16) | (b[1] << 8) | b[2]
            typ = b[3]
            size = h["last_page_size"] if p == h["num_pages"] - 1 else h["page_size"]
            off = data_base + (num - 1) * h["page_size"] if num else None
            self.pages.append({"num": num, "type": typ, "file_off": off, "size": size})

    def _parse_fixups(self):
        h, d = self.hdr, self.data
        fpt = self.le_off + h["fixup_page_table_off"]
        frt = self.le_off + h["fixup_record_table_off"]
        offs = struct.unpack_from("<%dI" % (h["num_pages"] + 1), d, fpt)
        self.fixups = []
        for page in range(h["num_pages"]):
            p, end = frt + offs[page], frt + offs[page + 1]
            while p < end:
                src, flg = d[p], d[p + 1]
                p += 2
                stype = src & 0x0F
                has_list = bool(src & 0x20)
                if has_list:
                    count = d[p]
                    p += 1
                else:
                    srcoff = struct.unpack_from("<h", d, p)[0]
                    p += 2
                ttype = flg & 3
                rec = {"page": page + 1, "src_type": SRC_TYPES.get(stype, hex(stype)),
                       "alias": bool(src & 0x10)}
                if ttype == 0:  # internal reference
                    if flg & 0x40:
                        obj = struct.unpack_from("<H", d, p)[0]
                        p += 2
                    else:
                        obj = d[p]
                        p += 1
                    rec["target_object"] = obj
                    if stype != 0x2:
                        if flg & 0x10:
                            rec["target_offset"] = struct.unpack_from("<I", d, p)[0]
                            p += 4
                        else:
                            rec["target_offset"] = struct.unpack_from("<H", d, p)[0]
                            p += 2
                else:
                    raise ValueError("import/entry-table fixups not supported (type %d) at page %d"
                                     % (ttype, page + 1))
                if flg & 0x04:  # additive
                    if flg & 0x20:
                        rec["additive"] = struct.unpack_from("<I", d, p)[0]
                        p += 4
                    else:
                        rec["additive"] = struct.unpack_from("<H", d, p)[0]
                        p += 2
                if has_list:
                    srcs = list(struct.unpack_from("<%dh" % count, d, p))
                    p += 2 * count
                else:
                    srcs = [srcoff]
                for s in srcs:
                    r = dict(rec)
                    r["src_off"] = s
                    self.fixups.append(r)

    # -- building memory images -------------------------------------------
    def page_object(self, page):
        for o in self.objects:
            if o["page_map_index"] <= page < o["page_map_index"] + o["page_count"]:
                return o
        return None

    def object_images(self):
        """Return {obj_index: bytearray} with fixups applied at preferred bases."""
        ps = self.hdr["page_size"]
        imgs = {}
        for o in self.objects:
            img = bytearray(o["virtual_size"])
            for k in range(o["page_count"]):
                pg = self.pages[o["page_map_index"] - 1 + k]
                if pg["type"] != 0 or pg["file_off"] is None:
                    continue  # zero-filled / invalid
                chunk = self.data[pg["file_off"]:pg["file_off"] + pg["size"]]
                start = k * ps
                n = max(0, min(len(chunk), len(img) - start))
                img[start:start + n] = chunk[:n]
            imgs[o["index"]] = img
        for f in self.fixups:
            o = self.page_object(f["page"])
            img = imgs[o["index"]]
            pos = (f["page"] - o["page_map_index"]) * ps + f["src_off"]
            tgt_obj = self.objects[f["target_object"] - 1]
            st = f["src_type"]
            if st == "off32":
                val = struct.pack("<I", (tgt_obj["base"] + f["target_offset"]) & 0xFFFFFFFF)
            elif st == "rel32":
                tgt = tgt_obj["base"] + f["target_offset"]
                val = struct.pack("<i", tgt - (o["base"] + pos + 4))
            elif st == "off16":
                val = struct.pack("<H", (tgt_obj["base"] + f["target_offset"]) & 0xFFFF)
            elif st == "sel16":
                # Selectors are assigned at run time; leave a recognisable marker.
                val = struct.pack("<H", 0xF000 | f["target_object"])
            elif st == "ptr16:32":
                val = struct.pack("<IH", tgt_obj["base"] + f["target_offset"],
                                  0xF000 | f["target_object"])
            else:
                raise ValueError("unhandled fixup type %s" % st)
            # Records that straddle a page boundary appear in both pages with a
            # clipped offset; write only the bytes that fall inside the object.
            for i, b in enumerate(val):
                if 0 <= pos + i < len(img):
                    img[pos + i] = b
        return imgs


def write_elf(path, le, imgs):
    """Minimal ELF32 executable: one PT_LOAD per object, plus section headers so
    tools show obj1/obj2/obj3 by name."""
    objs = le.objects
    entry = objs[le.hdr["eip_object"] - 1]["base"] + le.hdr["eip"]
    ehsize, phsize, shsize = 52, 32, 40
    shstr = b"\0" + b"".join(("obj%d\0" % o["index"]).encode() for o in objs) + b".shstrtab\0"
    data_off = ehsize + phsize * len(objs)
    layout, cur = [], data_off
    for o in objs:
        cur = (cur + 0xFFF) & ~0xFFF
        layout.append(cur)
        cur += len(imgs[o["index"]])
    shstr_off = cur
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    nsec = len(objs) + 2
    out = bytearray(sh_off + shsize * nsec)
    out[0:16] = b"\x7fELF\x01\x01\x01" + b"\0" * 9
    struct.pack_into("<HHIIIIIHHHHHH", out, 16, 2, 3, 1, entry, ehsize, sh_off, 0,
                     ehsize, phsize, len(objs), shsize, nsec, nsec - 1)
    for i, o in enumerate(objs):
        img = imgs[o["index"]]
        pf = (4 if o["readable"] else 0) | (2 if o["writable"] else 0) | (1 if o["executable"] else 0)
        struct.pack_into("<IIIIIIII", out, ehsize + phsize * i, 1, layout[i], o["base"], o["base"],
                         len(img), len(img), pf, 0x1000)
        out[layout[i]:layout[i] + len(img)] = img
    out[shstr_off:shstr_off + len(shstr)] = shstr
    name_off = 1
    for i, o in enumerate(objs):
        shf = 2 | (1 if o["writable"] else 0) | (4 if o["executable"] else 0)  # ALLOC|WRITE|EXEC
        struct.pack_into("<IIIIIIIIII", out, sh_off + shsize * (i + 1), name_off, 1, shf,
                         o["base"], layout[i], len(imgs[o["index"]]), 0, 0, 16, 0)
        name_off += len("obj%d\0" % o["index"])
    struct.pack_into("<IIIIIIIIII", out, sh_off + shsize * (nsec - 1), name_off, 3, 0, 0,
                     shstr_off, len(shstr), 0, 0, 1, 0)
    with open(path, "wb") as f:
        f.write(out)
    return entry


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", help="path to BARENA.EXE")
    ap.add_argument("--out", default="extracted", help="output directory")
    a = ap.parse_args()
    le = LEImage(open(a.exe, "rb").read())
    os.makedirs(a.out, exist_ok=True)
    imgs = le.object_images()
    for i, img in imgs.items():
        with open(os.path.join(a.out, "obj%d.bin" % i), "wb") as f:
            f.write(img)
    entry = write_elf(os.path.join(a.out, "image.elf"), le, imgs)
    info = {"mz_offset": le.mz_off, "le_offset": le.le_off, "header": le.hdr,
            "objects": le.objects, "entry": entry, "num_fixups": len(le.fixups)}
    with open(os.path.join(a.out, "info.json"), "w") as f:
        json.dump(info, f, indent=1)
    with open(os.path.join(a.out, "fixups.json"), "w") as f:
        json.dump(le.fixups, f)
    from collections import Counter
    print("LE at %#x (bound MZ at %#x), entry %#x" % (le.le_off, le.mz_off, entry))
    for o in le.objects:
        print("  obj%d base=%#08x size=%#07x %s%s%s" % (
            o["index"], o["base"], o["virtual_size"], "R" if o["readable"] else "-",
            "W" if o["writable"] else "-", "X" if o["executable"] else "-"))
    print("  fixups:", dict(Counter(f["src_type"] for f in le.fixups)))


if __name__ == "__main__":
    sys.exit(main())
