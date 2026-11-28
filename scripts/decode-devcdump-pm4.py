#!/usr/bin/env python3
import argparse, base64, re, struct, sys

def extract_bo(path, iova):
    in_bo = in_data = False
    lines = []
    target = int(iova, 16)
    with open(path, 'r', errors='ignore') as f:
        for line in f:
            m = re.match(r"\s*- iova: (0x[0-9a-fA-F]+)", line)
            if m:
                if in_bo and in_data:
                    break
                in_bo = int(m.group(1), 16) == target
                in_data = False
                continue
            if in_bo and line.strip().startswith('data: !!ascii85 |'):
                in_data = True
                continue
            if in_data:
                if re.match(r"\s{5,}\S", line) and not line.strip().startswith('- '):
                    lines.append(line.strip())
                else:
                    break
    if not lines:
        raise SystemExit(f'BO {iova} not found')
    return base64.a85decode(''.join(lines), adobe=False)

def odd_parity(v):
    return (bin(v).count('1') & 1) ^ 1

def pkt(w):
    t = (w >> 28) & 0xf
    if w == 0x80000000:
        return ('type2_nop', 0, 1, True)
    if t == 7:
        op = (w >> 16) & 0x7f
        cnt = w & 0x3fff
        ok = (((w >> 23) & 1) == odd_parity(op)) and (((w >> 15) & 1) == odd_parity(cnt)) and (((w >> 24) & 0xf) == 0) and (((w >> 14) & 1) == 0)
        return (f'type7 op=0x{op:02x}', op, cnt + 1, ok)
    if t == 4:
        reg = (w >> 8) & 0x3ffff
        cnt = w & 0x7f
        ok = (((w >> 27) & 1) == odd_parity(reg)) and (((w >> 7) & 1) == odd_parity(cnt)) and (((w >> 26) & 1) == 0)
        return (f'type4 reg=0x{reg:05x}', reg, cnt + 1, ok)
    return (f'INVALID_HDR(0x{w:08x})', 0, 1, False)

ap = argparse.ArgumentParser()
ap.add_argument('devcd')
ap.add_argument('iova')
ap.add_argument('--le', action='store_true')
ap.add_argument('--start-dw', type=lambda x: int(x, 0), default=0)
ap.add_argument('--end-dw', type=lambda x: int(x, 0), default=None)
ap.add_argument('--fault-dw', type=lambda x: int(x, 0), default=None)
args = ap.parse_args()
b = extract_bo(args.devcd, args.iova)
words = len(b) // 4
end_dw = args.end_dw if args.end_dw is not None else words
i = args.start_dw
while i < min(end_dw, words):
    w = struct.unpack_from('<I' if args.le else '>I', b, i * 4)[0]
    d, op, sz, ok = pkt(w)
    sz = max(sz, 1)
    mark = ''
    if args.fault_dw is not None and i <= args.fault_dw < i + sz:
        mark = ' <==FAULT'
    bad = '' if ok else ' BAD'
    print(f'{i*4:05x} dw{i:5d}: {w:08x} {d} sz={sz}{bad}{mark}')
    i += sz
