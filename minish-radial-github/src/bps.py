# Minimal BPS patch creator + applier (beat format) with round-trip verification.
import sys, zlib

def enc(n):
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        n -= 1

def dec(buf, i):
    data, shift = 0, 1
    while True:
        x = buf[i]; i += 1
        data += (x & 0x7F) * shift
        if x & 0x80:
            return data, i
        shift <<= 7
        data += shift

def create(src, dst):
    p = bytearray(b"BPS1") + enc(len(src)) + enc(len(dst)) + enc(0)
    i, n = 0, len(dst)
    while i < n:
        same = i < len(src) and src[i] == dst[i]
        j = i
        while j < n and (j < len(src) and src[j] == dst[j]) == same:
            j += 1
        length = j - i
        if same:
            p += enc(((length - 1) << 2) | 0)
        else:
            p += enc(((length - 1) << 2) | 1) + dst[i:j]
        i = j
    p += zlib.crc32(src).to_bytes(4, "little") + zlib.crc32(dst).to_bytes(4, "little")
    p += zlib.crc32(p).to_bytes(4, "little")
    return bytes(p)

def apply(src, patch):
    assert patch[:4] == b"BPS1"
    assert zlib.crc32(patch[:-4]) == int.from_bytes(patch[-4:], "little"), "patch crc"
    assert zlib.crc32(src) == int.from_bytes(patch[-12:-8], "little"), "wrong source ROM"
    i = 4
    ss, i = dec(patch, i); ts, i = dec(patch, i); ms, i = dec(patch, i); i += ms
    out = bytearray(ts); o = 0; srel = trel = 0
    end = len(patch) - 12
    while i < end:
        d, i = dec(patch, i)
        cmd, length = d & 3, (d >> 2) + 1
        if cmd == 0:
            out[o:o + length] = src[o:o + length]
        elif cmd == 1:
            out[o:o + length] = patch[i:i + length]; i += length
        elif cmd == 2:
            d, i = dec(patch, i); srel += (-1 if d & 1 else 1) * (d >> 1)
            out[o:o + length] = src[srel:srel + length]; srel += length
        else:
            d, i = dec(patch, i); trel += (-1 if d & 1 else 1) * (d >> 1)
            for k in range(length):
                out[o + k] = out[trel]; trel += 1
        o += length
    assert zlib.crc32(out) == int.from_bytes(patch[-8:-4], "little"), "target crc"
    return bytes(out)

if __name__ == "__main__":
    src = open(sys.argv[1], "rb").read()
    dst = open(sys.argv[2], "rb").read()
    patch = create(src, dst)
    assert apply(src, patch) == dst
    open(sys.argv[3], "wb").write(patch)
    print("bps ok: %d bytes" % len(patch))
