"""
core.py — CRUSH's compression math. No zlib, no external libraries.

Two pieces, both built from scratch:

1. A binary range coder (a form of arithmetic coding). It squeezes a stream
   of yes/no decisions into very close to their true information content:
   a bit the model is 99% sure about costs ~0.014 bits, not a whole bit.
   Carryless 32-bit integer version in the fpaq0 lineage (Matt Mahoney) —
   small and exactly reversible.

2. An adaptive context model. Each byte is coded one bit at a time, most
   significant bit first. For every bit we predict P(bit = 1) from a context —
   here the *previous whole byte* plus the bits of the current byte seen so far
   (an "order-1" model). The prediction is a 12-bit probability that we nudge
   toward what actually happened after each bit, so the model learns the file
   as it reads it. Encoder and decoder run the identical model and updates, so
   they stay in perfect lockstep and the round-trip is bit-exact.

The ratio you get is real entropy coding over a learned model — not a lookup
table of someone else's algorithm. See README.md for how to push it further
(higher orders, mixing several models).
"""

from array import array

# 12-bit probabilities: 0 and 4096 are never used (they'd mean "impossible").
PBITS = 12
PSCALE = 1 << PBITS          # 4096
PHALF = PSCALE >> 1          # 2048  (= P 0.5, the starting guess)
ADAPT = 5                    # learning rate: smaller = faster, noisier
CTX_SIZE = 256 * 256         # order-1: prev byte (256) * partial-byte node (256)
MASK32 = 0xFFFFFFFF


class RangeEncoder:
    """Writes bits into an ever-narrowing [x1, x2] interval; emits settled bytes."""

    def __init__(self):
        self.x1 = 0
        self.x2 = MASK32
        self.out = bytearray()

    def encode(self, bit, p):
        # p = P(bit == 1), 12-bit, in [1, 4095]. Split the interval at xmid.
        xmid = self.x1 + (((self.x2 - self.x1) >> PBITS) * p)
        if bit:
            self.x2 = xmid
        else:
            self.x1 = xmid + 1
        # While the top byte of x1 and x2 agree, it can never change again: emit it.
        while ((self.x1 ^ self.x2) & 0xFF000000) == 0:
            self.out.append((self.x2 >> 24) & 0xFF)
            self.x1 = (self.x1 << 8) & MASK32
            self.x2 = ((self.x2 << 8) & MASK32) | 0xFF

    def finish(self):
        # Flush enough of x1 to disambiguate the final interval.
        for _ in range(4):
            self.out.append((self.x1 >> 24) & 0xFF)
            self.x1 = (self.x1 << 8) & MASK32
        return bytes(self.out)


class RangeDecoder:
    """Mirror of the encoder: reads the settled bytes back, same interval math."""

    def __init__(self, data):
        self.x1 = 0
        self.x2 = MASK32
        self.data = data
        self.pos = 0
        self.x = 0
        for _ in range(4):
            self.x = ((self.x << 8) | self._get()) & MASK32

    def _get(self):
        if self.pos < len(self.data):
            b = self.data[self.pos]
            self.pos += 1
            return b
        return 0            # past the end: pad with zeros (matches the flush)

    def decode(self, p):
        xmid = self.x1 + (((self.x2 - self.x1) >> PBITS) * p)
        if self.x <= xmid:
            bit = 1
            self.x2 = xmid
        else:
            bit = 0
            self.x1 = xmid + 1
        while ((self.x1 ^ self.x2) & 0xFF000000) == 0:
            self.x1 = (self.x1 << 8) & MASK32
            self.x2 = ((self.x2 << 8) & MASK32) | 0xFF
            self.x = ((self.x << 8) | self._get()) & MASK32
        return bit


def _new_model():
    # Every context starts at P 0.5 (= PHALF). 12-bit probabilities.
    return array('H', [PHALF]) * CTX_SIZE


def compress(data):
    """bytes -> compressed bytes (the raw coded stream, no header)."""
    enc = RangeEncoder()
    t = _new_model()
    prev = 0
    for byte in data:
        ctx = prev << 8
        node = 1                                   # partial byte, leading-1 sentinel
        for shift in (7, 6, 5, 4, 3, 2, 1, 0):
            bit = (byte >> shift) & 1
            idx = ctx | node
            p = t[idx]
            enc.encode(bit, p)
            if bit:
                t[idx] = p + ((PSCALE - p) >> ADAPT)
            else:
                t[idx] = p - (p >> ADAPT)
            node = (node << 1) | bit
        prev = byte
    return enc.finish()


def decompress(comp, out_size):
    """compressed bytes + known original length -> the original bytes."""
    dec = RangeDecoder(comp)
    t = _new_model()
    prev = 0
    out = bytearray(out_size)
    for i in range(out_size):
        ctx = prev << 8
        node = 1
        for _ in range(8):
            idx = ctx | node
            p = t[idx]
            bit = dec.decode(p)
            if bit:
                t[idx] = p + ((PSCALE - p) >> ADAPT)
            else:
                t[idx] = p - (p >> ADAPT)
            node = (node << 1) | bit
        byte = node & 0xFF                          # node is now 256..511
        out[i] = byte
        prev = byte
    return bytes(out)
