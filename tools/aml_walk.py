#!/usr/bin/env python3
"""Walk a DSDT dump on the host and say how its PCI interrupt routing is
written -- the measurement behind kernel/acpi/'s `_PRT` reader.

    python3 tools/acpi_dump.py DSDT --out dsdt.txt        # from a guest
    python3 tools/remote.py --host <ip> exec "acpi --dump DSDT" > dsdt.txt
    python3 tools/aml_walk.py dsdt.txt                     # this
    python3 tools/aml_walk.py dsdt.txt --tree              # the namespace

WHY A SECOND WALKER. `kernel/acpi/aml.c` walks the same grammar in ring
0, but a design question -- "is `_PRT` a static package or a method,
and do its link devices carry a static `_CRS`?" -- has to be answered
from every machine's tables BEFORE deciding what the kernel decodes,
and no AML disassembler is installed here. This is also the oracle for
the kernel's constant-DataObject decoder: the two share no code.

WHAT IT REPORTS, per `_PRT`:
  - `Name`: the package decoded, one line per entry.
  - `Method`: whether the body is the canonical
        If (<pred>) { Return (A) } [Else] { Return (B) }
    with A and B named, and both packages decoded when they are Names.
  - For every link device a package entry names: whether its `_CRS`
    is a Name (decoded: the IRQ/Interrupt descriptor) or a Method.

Deliberately NOT an interpreter: a Name whose value is an expression
is reported as `<expr>`, never guessed.
"""
import argparse
import struct
import sys

# --- the bytes ---------------------------------------------------------------


def load_dump(path):
    """`acpi --dump` output: `  0000  44 53 ...` rows, or a raw table."""
    data = open(path, "rb").read()
    # A raw table starts with the signature AND a length that fits; the
    # text dump starts with `DSDT: <n> bytes`, which only looks like one.
    if data[:4] in (b"DSDT", b"SSDT") and len(data) >= 8 and \
            struct.unpack_from("<I", data, 4)[0] <= len(data):
        return data
    out = bytearray()
    for line in data.decode("utf-8", "replace").splitlines():
        parts = line.split()
        if len(parts) < 2 or not all(len(p) == 2 for p in parts[1:]) or len(parts[0]) < 4:
            continue
        try:
            int(parts[0], 16)
            out += bytes(int(p, 16) for p in parts[1:])
        except ValueError:
            continue
    return bytes(out)


# --- the grammar, as far as declarations go ------------------------------------

def pkg_length(b, o):
    lead = b[o]
    n = (lead >> 6) & 3
    if n == 0:
        return lead & 0x3F, 1
    v = lead & 0x0F
    for i in range(n):
        v |= b[o + 1 + i] << (4 + 8 * i)
    return v, 1 + n


def name_string(b, o):
    """Returns (segments as a list of 4-char names, root?, ups, used)."""
    start = o
    root = False
    ups = 0
    if b[o] == 0x5C:
        root = True
        o += 1
    while b[o] == 0x5E:
        ups += 1
        o += 1
    if b[o] == 0x00:
        return [], root, ups, o + 1 - start
    if b[o] == 0x2E:
        segs = [b[o + 1:o + 5], b[o + 5:o + 9]]
        o += 9
    elif b[o] == 0x2F:
        n = b[o + 1]
        segs = [b[o + 2 + 4 * i:o + 6 + 4 * i] for i in range(n)]
        o += 2 + 4 * n
    else:
        segs = [b[o:o + 4]]
        o += 4
    return [s.decode("ascii", "replace") for s in segs], root, ups, o - start


def is_name_lead(c):
    return c == 0x5C or c == 0x5E or c == 0x2E or c == 0x2F or c == 0x5F or 0x41 <= c <= 0x5A


class Node:
    def __init__(self, path, kind, body=None):
        self.path = path
        self.kind = kind
        self.body = body   # (offset, length) into the table for Name/Method


def resolve(scope, segs, root, ups):
    if root:
        base = []
    else:
        base = list(scope)
        for _ in range(ups):
            if base:
                base.pop()
    return base + segs


def data_len(b, o):
    """Length of a constant DataObject at o, or 0 for an expression."""
    op = b[o]
    if op in (0x00, 0x01, 0xFF):
        return 1
    if op == 0x0A:
        return 2
    if op == 0x0B:
        return 3
    if op == 0x0C:
        return 5
    if op == 0x0E:
        return 9
    if op == 0x0D:
        e = b.index(b"\0", o + 1)
        return e + 1 - o
    if op in (0x11, 0x12, 0x13):
        n, used = pkg_length(b, o + 1)
        return 1 + n
    return 0


# Expression opcodes and how many TermArgs/Targets follow them -- enough to
# SKIP one without evaluating it. Entries are (n_args) where every arg is
# a TermArg or a Target (both skippable by the same rule). Anything not
# listed stops the walk, which is reported rather than guessed.
ARITY = {
    0x70: 2, 0x71: 1, 0x72: 3, 0x73: 4, 0x74: 3, 0x75: 1, 0x76: 1, 0x77: 3,
    0x78: 4, 0x79: 3, 0x7A: 3, 0x7B: 3, 0x7C: 3, 0x7D: 3, 0x7E: 3, 0x7F: 3,
    0x80: 2, 0x81: 2, 0x82: 2, 0x83: 2, 0x84: 3, 0x85: 3, 0x86: 2, 0x87: 1,
    0x88: 3, 0x89: 4, 0x8A: 3, 0x8B: 3, 0x8C: 3, 0x8D: 3, 0x8E: 3, 0x8F: 3,
    0x90: 2, 0x91: 2, 0x92: 1, 0x93: 2, 0x94: 2, 0x95: 2, 0x96: 2, 0x97: 2,
    0x98: 2, 0x99: 2, 0x9C: 3, 0x9D: 2, 0x9E: 3, 0x9F: 2,
}
EXT_ARITY = {0x12: 1, 0x23: 2, 0x27: 1, 0x28: 2, 0x29: 3, 0x33: 0, 0x32: 1, 0x30: 0, 0x31: 0}


def skip_arg(b, o, end):
    """One TermArg or Target starting at o; returns the offset after it,
    or None when it is a shape this walker does not know."""
    if o >= end:
        return None
    op = b[o]
    dl = data_len(b, o)
    if dl:
        return o + dl
    if is_name_lead(op):
        return o + name_string(b, o)[3]
    if 0x60 <= op <= 0x6E:   # Local0-7, Arg0-6
        return o + 1
    if op == 0x5B:
        ext = b[o + 1]
        if ext in EXT_ARITY:
            p = o + 2
            for _ in range(EXT_ARITY[ext]):
                p = skip_arg(b, p, end)
                if p is None:
                    return None
            return p
        return None
    if op in ARITY:
        p = o + 1
        for _ in range(ARITY[op]):
            p = skip_arg(b, p, end)
            if p is None:
                return None
        return p
    if op == 0x11 or op == 0x12 or op == 0x13:
        n, used = pkg_length(b, o + 1)
        return o + 1 + n
    return None


def walk(b, o, end, scope, nodes):
    while o < end:
        op = b[o]
        if op == 0x10:  # Scope
            n, used = pkg_length(b, o + 1)
            segs, root, ups, nl = name_string(b, o + 1 + used)
            path = resolve(scope, segs, root, ups)
            nodes.append(Node(path, "scope"))
            walk(b, o + 1 + used + nl, o + 1 + n, path, nodes)
            o += 1 + n
        elif op == 0x08:  # Name
            segs, root, ups, nl = name_string(b, o + 1)
            path = resolve(scope, segs, root, ups)
            dl = data_len(b, o + 1 + nl)
            nodes.append(Node(path, "name", (o + 1 + nl, dl)))
            if dl == 0:
                return o  # an expression: stop this term list honestly
            o += 1 + nl + dl
        elif op == 0x14:  # Method
            n, used = pkg_length(b, o + 1)
            segs, root, ups, nl = name_string(b, o + 1 + used)
            path = resolve(scope, segs, root, ups)
            body = o + 1 + used + nl + 1  # after the flags byte
            nodes.append(Node(path, "method", (body, o + 1 + n - body)))
            o += 1 + n
        elif op == 0x06:  # Alias
            _s, _r, _u, l1 = name_string(b, o + 1)
            _s, _r, _u, l2 = name_string(b, o + 1 + l1)
            o += 1 + l1 + l2
        elif op == 0x15:  # External
            _s, _r, _u, l1 = name_string(b, o + 1)
            o += 1 + l1 + 2
        elif op == 0x5B:
            ext = b[o + 1]
            if ext in (0x82, 0x83, 0x84, 0x85):  # Device, Processor, PowerRes, ThermalZone
                n, used = pkg_length(b, o + 2)
                segs, root, ups, nl = name_string(b, o + 2 + used)
                path = resolve(scope, segs, root, ups)
                kind = {0x82: "device", 0x83: "cpu", 0x84: "power", 0x85: "thermal"}[ext]
                nodes.append(Node(path, kind))
                trailer = {0x82: 0, 0x83: 6, 0x84: 3, 0x85: 0}[ext]
                walk(b, o + 2 + used + nl + trailer, o + 2 + n, path, nodes)
                o += 2 + n
            elif ext in (0x81, 0x86, 0x87):  # Field, IndexField, BankField
                n, used = pkg_length(b, o + 2)
                o += 2 + n
            elif ext == 0x80:  # OpRegion: Name, space byte, two TermArgs
                _s, _r, _u, nl = name_string(b, o + 2)
                p = o + 2 + nl + 1
                for _ in range(2):
                    p = skip_arg(b, p, end)
                    if p is None:
                        return o
                o = p
            elif ext in (0x01, 0x02):  # Mutex, Event
                _s, _r, _u, nl = name_string(b, o + 2)
                o += 2 + nl + (1 if ext == 0x01 else 0)
            elif ext == 0x13:  # CreateField
                return o
            else:
                return o
        elif op in (0xA0, 0xA1, 0xA2):  # If / Else / While at term level: skipped whole
            n, used = pkg_length(b, o + 1)
            o += 1 + n
        else:
            p = skip_arg(b, o, end)   # CreateXField, Store, a bare method call...
            if p is None:
                return o
            o = p
    return o


# --- decoding ------------------------------------------------------------------

def decode_integer(b, o):
    op = b[o]
    if op == 0x00:
        return 0, 1
    if op == 0x01:
        return 1, 1
    if op == 0xFF:
        return 0xFFFFFFFFFFFFFFFF, 1
    if op == 0x0A:
        return b[o + 1], 2
    if op == 0x0B:
        return struct.unpack_from("<H", b, o + 1)[0], 3
    if op == 0x0C:
        return struct.unpack_from("<I", b, o + 1)[0], 5
    if op == 0x0E:
        return struct.unpack_from("<Q", b, o + 1)[0], 9
    return None, 0


def decode_object(b, o):
    """A constant DataObject -> Python value; NameString -> ('ref', path)."""
    op = b[o]
    v, used = decode_integer(b, o)
    if used:
        return v, used
    if op == 0x0D:
        e = b.index(b"\0", o + 1)
        return b[o + 1:e].decode("ascii", "replace"), e + 1 - o
    if op == 0x11:  # Buffer
        n, used = pkg_length(b, o + 1)
        size, su = decode_integer(b, o + 1 + used)
        if not su:
            return "<buffer of expression length>", 1 + n
        start = o + 1 + used + su
        return ("buffer", bytes(b[start:o + 1 + n])), 1 + n
    if op == 0x12:  # Package
        n, used = pkg_length(b, o + 1)
        count = b[o + 1 + used]
        p = o + 2 + used
        items = []
        for _ in range(count):
            if p >= o + 1 + n:
                break
            item, iu = decode_object(b, p)
            if not iu:
                items.append("<expr>")
                break
            items.append(item)
            p += iu
        return items, 1 + n
    if is_name_lead(op):
        segs, root, ups, nl = name_string(b, o)
        return ("ref", segs, root, ups), nl
    return None, 0


def decode_crs(buf):
    """The IRQ/Interrupt descriptors in a resource buffer."""
    out = []
    i = 0
    while i < len(buf):
        t = buf[i]
        if t & 0x80:
            ln = struct.unpack_from("<H", buf, i + 1)[0]
            body = buf[i + 3:i + 3 + ln]
            if t == 0x89 and len(body) >= 6:
                flags = body[0]
                n = body[1]
                irqs = [struct.unpack_from("<I", body, 2 + 4 * k)[0] for k in range(n) if 6 + 4 * k <= len(body)]
                out.append(f"Interrupt({'level' if not flags & 2 else 'edge'},"
                           f"{'low' if flags & 4 else 'high'}) {irqs}")
            i += 3 + ln
        else:
            ln = t & 7
            if (t >> 3) == 0x04:  # IRQ descriptor
                mask = struct.unpack_from("<H", buf, i + 1)[0]
                irqs = [k for k in range(16) if mask & (1 << k)]
                out.append(f"IRQ {irqs}")
            if (t >> 3) == 0x0F:
                break
            i += 1 + ln
    return out


def fmt_ref(item):
    if isinstance(item, tuple) and item and item[0] == "ref":
        return ("\\" if item[2] else "^" * item[3]) + ".".join(item[1])
    return repr(item)


# --- the report --------------------------------------------------------------------

def find(nodes, path):
    for n in nodes:
        if n.path == path:
            return n
    return None


def find_relative(nodes, scope, name):
    """ACPI's search rules: the name in scope, then each parent."""
    s = list(scope)
    while True:
        n = find(nodes, s + [name])
        if n:
            return n
        if not s:
            return None
        s.pop()


def method_returns(b, body, length):
    """Recognise `If (pred) { Return (A) } [Else {] Return (B) [}]`.
    Returns (pred_text, A, B) or None."""
    o = body
    end = body + length
    pred = None
    ret_a = None
    ret_b = None

    def ret_name(p):
        if p < end and b[p] == 0xA4 and p + 1 < end and is_name_lead(b[p + 1]):
            segs, root, ups, nl = name_string(b, p + 1)
            return ("\\" if root else "^" * ups) + ".".join(segs), p + 1 + nl
        return None, p

    if o < end and b[o] == 0xA0:
        n, used = pkg_length(b, o + 1)
        p = o + 1 + used
        # the predicate: LEqual(X, Y) / LNot(X) / a bare name / an integer
        if b[p] == 0x93:
            x, xu = decode_object(b, p + 1)
            y, yu = decode_object(b, p + 1 + xu)
            pred = f"LEqual({fmt_ref(x)}, {fmt_ref(y)})"
            p += 1 + xu + yu
        elif b[p] == 0x92:
            x, xu = decode_object(b, p + 1)
            pred = f"LNot({fmt_ref(x)})"
            p += 1 + xu
        else:
            x, xu = decode_object(b, p)
            if not xu:
                return None
            pred = fmt_ref(x)
            p += xu
        ret_a, p2 = ret_name(p)
        if ret_a is None:
            return None
        o = o + 1 + n
        if o < end and b[o] == 0xA1:
            n2, used2 = pkg_length(b, o + 1)
            ret_b, _ = ret_name(o + 1 + used2)
            o = o + 1 + n2
        else:
            ret_b, o = ret_name(o)
        if ret_b is None:
            return None
        return pred, ret_a, ret_b
    ret_b, o = ret_name(o)
    if ret_b is not None:
        return "", None, ret_b
    return None


def package_of(b, node):
    """The constant package a Name holds, or a Method returns outright
    (`Method (AR00) { Return (Package {...}) }`, the Lenovo's shape)."""
    if node.kind == "name":
        return decode_object(b, node.body[0])[0]
    if node.kind == "method":
        o, ln = node.body
        if ln > 1 and b[o] == 0xA4:
            v, used = decode_object(b, o + 1)
            if used and o + 1 + used == o + ln:
                if isinstance(v, tuple) and v[0] == "ref":
                    # A method body's scope is the METHOD, so `^^X` from
                    # inside \_SB.PCI0.AR00 names \_SB.X.
                    target = resolve_ref(nodes_global, node.path, v)
                    return package_of(b, target) if target and target is not node else None
                return v
    return None


nodes_global = []


def resolve_ref(nodes, scope, ref):
    _tag, segs, root, ups = ref
    if root or ups or len(segs) > 1:
        return find(nodes, resolve(scope, segs, root, ups))
    return find_relative(nodes, scope, segs[0])


def report_package(b, nodes, node, label):
    pkg = package_of(b, node)
    if not isinstance(pkg, list):
        print(f"    {label}: not a package ({pkg!r})")
        return
    print(f"    {label}: {len(pkg)} entries")
    sources = {}
    for e in pkg:
        if not isinstance(e, list) or len(e) != 4:
            print(f"      <odd entry {e!r}>")
            continue
        addr, pin, src, idx = e
        dev = (addr >> 16) & 0xFFFF if isinstance(addr, int) else addr
        s = fmt_ref(src) if isinstance(src, tuple) else src
        print(f"      dev {dev:#04x} INT{'ABCD'[pin] if isinstance(pin, int) and pin < 4 else pin}"
              f" -> {'GSI ' + str(idx) if src == 0 else s + ' index ' + str(idx)}")
        if isinstance(src, tuple):
            sources[s] = src
    for s, ref in sources.items():
        link = find_relative(nodes, node.path[:-1], ref[1][-1]) if len(ref[1]) == 1 and not ref[2] else find(nodes, ref[1])
        if not link:
            print(f"      link {s}: NOT FOUND")
            continue
        crs = find(nodes, link.path + ["_CRS"])
        if not crs:
            print(f"      link {s}: no _CRS")
        else:
            v = package_of(b, crs)
            if v is None:
                print(f"      link {s}: _CRS is a METHOD (needs execution)")
            else:
                desc = decode_crs(v[1]) if isinstance(v, tuple) and v[0] == "buffer" else v
                print(f"      link {s}: _CRS is a {crs.kind} -> {desc}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dump")
    ap.add_argument("--tree", action="store_true", help="print the namespace")
    args = ap.parse_args()

    b = load_dump(args.dump)
    if len(b) < 36 or b[:4] not in (b"DSDT", b"SSDT"):
        sys.exit(f"aml_walk: {args.dump}: not a DSDT/SSDT dump")
    length = struct.unpack_from("<I", b, 4)[0]
    if length > len(b):
        sys.exit(f"aml_walk: {args.dump}: table says {length} bytes, dump holds {len(b)} -- truncated")
    # The ACPI checksum: every byte of the table sums to 0 mod 256. A
    # dump that lost bytes in transit is refused here, not misread.
    if sum(b[:length]) & 0xFF:
        sys.exit(f"aml_walk: {args.dump}: checksum mismatch -- the dump is corrupt "
                 f"(fetch it in ranges: acpi --dump DSDT --at N --len N)")
    nodes = nodes_global
    stopped = walk(b, 36, length, [], nodes)
    print(f"aml_walk: {b[:4].decode()} {length} bytes, {len(nodes)} node(s)"
          + (f", walk stopped at {stopped:#x} on opcode {b[stopped]:#04x}"
             + (f" {b[stopped + 1]:#04x}" if b[stopped] == 0x5B else "")
             if stopped < length else ""))
    if args.tree:
        for n in nodes:
            print(f"  {n.kind:7s} \\{'.'.join(n.path)}")

    prts = [n for n in nodes if n.path and n.path[-1] == "_PRT"]
    if not prts:
        print("  no _PRT in this table")
    for n in prts:
        print(f"  \\{'.'.join(n.path)}: {n.kind.upper()}")
        if n.kind == "name":
            report_package(b, nodes, n, "package")
        elif package_of(b, n) is not None:
            report_package(b, nodes, n, "package (returned outright)")
        else:
            shape = method_returns(b, n.body[0], n.body[1])
            if not shape:
                head = " ".join(f"{x:02x}" for x in b[n.body[0]:n.body[0] + 16])
                print(f"    body is not `If (pred) Return A / Return B`: {head} ...")
                continue
            pred, a, bname = shape
            print(f"    If ({pred}) Return ({a}) else Return ({bname})" if a else f"    Return ({bname})")
            for label, ref in (("A", a), ("B", bname)):
                if not ref:
                    continue
                target = find_relative(nodes, n.path[:-1], ref.lstrip("\\^").split(".")[-1])
                if not target:
                    print(f"    {label} = {ref}: NOT FOUND")
                elif package_of(b, target) is None:
                    print(f"    {label} = {ref}: a {target.kind} with no constant package")
                else:
                    report_package(b, nodes, target, f"{label} = {ref}"
                                   + (" (a Method returning a constant)" if target.kind == "method" else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
