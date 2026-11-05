#!/usr/bin/env python3
"""Write the PA19 patches the mspatcha client applies, with the old and new files they join.

The LZX DELTA streams are made here, from [MS-PATCH], by a small encoder that is told what to emit: literals, matches into the old file
or the new one, uncompressed runs. It covers the three block types, chunks of 32 KiB crossed by blocks and by uncompressed runs,
repeated and long matches and the x86 call translation; the headers cover two old files with ignored and retained ranges, a time stamp,
a patch between identical files and one that carries only its header.

usage: mspatcha_fixtures.py OUTPUT_DIR
"""

import heapq
import struct
import sys
import zlib
from pathlib import Path

CHUNK = 32768
NO_REBASE = 0x00040000
NO_TIMESTAMP = 0x00800000
LZX_B = 0x2


def crc32(data, crc=0):
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def unsigned_number(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if not value:
            out.append(byte | 0x80)
            return bytes(out)
        out.append(byte)


def signed_number(value):
    magnitude, sign = abs(value), 0x40 if value < 0 else 0
    first = magnitude & 0x3F
    magnitude >>= 6
    if not magnitude:
        return bytes([first | sign | 0x80])
    return bytes([first | sign]) + unsigned_number(magnitude)


def slot_layout(reference, size):
    """Slots of the window the decoder deduces, with each one's base and footer bits."""
    total = ((reference + CHUNK - 1) & ~(CHUNK - 1)) + size
    window, slots, step = 1 << 17, 34, 2
    while window < total and window < 8 << 20:
        if window >= 1 << 19:
            step = 4 if step == 2 else step * 2
        slots += step
        window <<= 1
    bases, extras, base = [], [], 0
    for slot in range(slots):
        extra = 0 if slot < 4 else min((slot - 2) // 2, 17)
        bases.append(base)
        extras.append(extra)
        base += 1 << extra
    return bases, extras


def code_lengths(frequencies, limit):
    """Huffman code lengths of the symbols that occur, none longer than limit."""
    used = [symbol for symbol, count in enumerate(frequencies) if count]
    lengths = [0] * len(frequencies)
    if len(used) == 1:
        lengths[used[0]] = 1
        return lengths
    heap = [(frequencies[symbol], index, [symbol]) for index, symbol in enumerate(used)]
    heapq.heapify(heap)
    order = len(heap)
    while len(heap) > 1:
        a, b = heapq.heappop(heap), heapq.heappop(heap)
        for symbol in a[2] + b[2]:
            lengths[symbol] += 1
        heapq.heappush(heap, (a[0] + b[0], order, a[2] + b[2]))
        order += 1
    if max(lengths) > limit:
        raise ValueError("code too long for the fixture")
    return lengths


def canonical(lengths):
    codes, code = [0] * len(lengths), 0
    for length in range(1, 17):
        for symbol, size in enumerate(lengths):
            if size == length:
                codes[symbol] = code
                code += 1
        code <<= 1
    return codes


class Stream:
    """Chunks of an LZX DELTA stream: bits go out as 16-bit little-endian words from the top bit down; uncompressed bytes as they are."""

    def __init__(self):
        self.chunks = [bytearray()]
        self.bits = self.count = 0

    def enter(self, index):
        """Starts chunk index when what comes next belongs to it."""
        while len(self.chunks) <= index:
            self.flush()
            self.chunks.append(bytearray())

    def put(self, value, count):
        for bit in range(count - 1, -1, -1):
            self.bits = self.bits << 1 | (value >> bit) & 1
            self.count += 1
            if self.count == 16:
                self.chunks[-1] += struct.pack("<H", self.bits)
                self.bits = self.count = 0

    def flush(self):
        if self.count:
            self.put(0, 16 - self.count)

    def offset(self):
        """Bytes from the stream's start, chunk sizes included."""
        return sum(2 + len(chunk) for chunk in self.chunks[:-1]) + 2 + len(self.chunks[-1])

    def data(self):
        self.flush()
        return b"".join(struct.pack("<H", len(chunk)) + bytes(chunk) for chunk in self.chunks)


class Encoder:
    def __init__(self, reference, size, translation=0):
        self.stream = Stream()
        self.reference, self.size, self.produced = reference, size, 0
        self.bases, self.extras = slot_layout(reference, size)
        self.main_size = 256 + 8 * len(self.bases)
        self.main_lengths = [0] * self.main_size
        self.length_lengths = [0] * 249
        self.recent = [1, 1, 1]
        self.stream.put(1 if translation else 0, 1)
        if translation:
            self.stream.put(translation >> 16, 16)
            self.stream.put(translation & 0xFFFF, 16)

    def at_output(self):
        """Whatever is written next makes output from here: it goes into the chunk of that output."""
        if self.produced < self.size:
            self.stream.enter(self.produced // CHUNK)

    def write_tree(self, lengths, previous, start, end):
        pretree_lengths = [4] * 12 + [5] * 8
        codes = canonical(pretree_lengths)
        for length in pretree_lengths:
            self.stream.put(length, 4)
        index = start
        while index < end:
            # Runs of zeros use the run symbols, the rest goes one by one.
            run = 0
            while index + run < end and lengths[index + run] == 0 and run < 51:
                run += 1
            if run >= 20:
                self.stream.put(codes[18], pretree_lengths[18])
                self.stream.put(run - 20, 5)
            elif run >= 4:
                self.stream.put(codes[17], pretree_lengths[17])
                self.stream.put(run - 4, 4)
            else:
                run = 1
                symbol = (previous[index] - lengths[index]) % 17
                self.stream.put(codes[symbol], pretree_lengths[symbol])
            index += run

    def elements(self, operations):
        """The symbols of a compressed block with what follows each of them."""
        out, recent = [], list(self.recent)
        for operation in operations:
            if operation[0] == "literal":
                out.extend(("literal", byte) for byte in operation[1])
                continue
            _, offset, length = operation
            if offset in recent:
                slot = recent.index(offset)
                recent[slot] = recent[0]
                footer = None
            else:
                formatted = offset + 2
                slot = max(index for index, base in enumerate(self.bases) if base <= formatted and index >= 3)
                footer = (formatted - self.bases[slot], self.extras[slot])
                recent[2], recent[1] = recent[1], recent[0]
            recent[0] = offset
            header = min(length - 2, 7)
            out.append(("match", 256 + slot * 8 + header, length, footer))
        return out, recent

    def block(self, block_type, operations):
        produced = sum(len(op[1]) if op[0] == "literal" else op[2] for op in operations)
        elements, recent = self.elements(operations)
        main_counts, length_counts = [0] * self.main_size, [0] * 249
        for element in elements:
            main_counts[element[1]] += 1
            if element[0] == "match" and element[2] >= 9:
                length_counts[min(element[2] - 9, 248)] += 1
        main_lengths = code_lengths(main_counts, 16)
        length_lengths = code_lengths(length_counts, 16) if any(length_counts) else [0] * 249
        self.at_output()
        self.stream.put(block_type, 3)
        self.stream.put(produced, 24)
        aligned_lengths = [3] * 8
        if block_type == 2:
            for length in aligned_lengths:
                self.stream.put(length, 3)
        self.write_tree(main_lengths, self.main_lengths, 0, 256)
        self.write_tree(main_lengths, self.main_lengths, 256, self.main_size)
        self.write_tree(length_lengths, self.length_lengths, 0, 249)
        self.main_lengths, self.length_lengths = main_lengths, length_lengths
        main_codes, length_codes, aligned_codes = canonical(main_lengths), canonical(length_lengths), canonical(aligned_lengths)
        for element in elements:
            self.at_output()
            self.stream.put(main_codes[element[1]], main_lengths[element[1]])
            if element[0] == "literal":
                self.produced += 1
                continue
            _, symbol, length, footer = element
            if length >= 9:
                self.stream.put(length_codes[min(length - 9, 248)], length_lengths[min(length - 9, 248)])
            if footer:
                value, extra = footer
                if block_type == 2 and extra >= 3:
                    self.stream.put(value >> 3, extra - 3)
                    self.stream.put(aligned_codes[value & 7], 3)
                else:
                    self.stream.put(value, extra)
            if length >= 257:
                if length <= 0x200:
                    self.stream.put(0, 1)
                    self.stream.put(length - 0x101, 8)
                elif length <= 0x600:
                    self.stream.put(2, 2)
                    self.stream.put(length - 0x201, 10)
                elif length <= 0x1600:
                    self.stream.put(6, 3)
                    self.stream.put(length - 0x601, 12)
                else:
                    self.stream.put(7, 3)
                    self.stream.put(length - 0x101, 15)
            self.produced += length
        self.recent = recent

    def raw(self, data, recent):
        self.at_output()
        self.stream.put(3, 3)
        self.stream.put(len(data), 24)
        # A header ending on a word boundary is followed by a word of zeros; otherwise the rest of the word is padding.
        if self.stream.count == 0:
            self.stream.put(0, 16)
        self.stream.flush()
        for value in recent:
            self.stream.put(value & 0xFFFF, 16)
            self.stream.put(value >> 16, 16)
        self.recent = list(recent)
        parity = self.stream.offset() & 1
        for byte in data:
            self.at_output()
            self.stream.chunks[-1].append(byte)
            self.produced += 1
        if self.stream.offset() & 1 != parity:
            self.stream.chunks[-1].append(0)

    def data(self):
        if self.produced != self.size:
            raise ValueError(f"stream makes {self.produced} bytes, not {self.size}")
        return self.stream.data()


def split_at_chunks(window, operations):
    """Literals and matches cut where the output crosses a chunk, so that every symbol starts and ends in the chunk it belongs to. A
    piece of one byte, too short for a match, becomes a literal."""
    out, buffer = [], bytearray(window)
    for operation in operations:
        if operation[0] == "literal":
            data = operation[1]
            while data:
                room = CHUNK - (len(buffer) - len(window)) % CHUNK
                out.append(("literal", data[:room]))
                buffer += data[:room]
                data = data[room:]
            continue
        _, offset, length = operation
        while length:
            room = CHUNK - (len(buffer) - len(window)) % CHUNK
            piece = min(room, length)
            if piece == 1:
                out.append(("literal", bytes([buffer[len(buffer) - offset]])))
            else:
                out.append(("match", offset, piece))
            for _ in range(piece):
                buffer.append(buffer[len(buffer) - offset])
            length -= piece
    return out


def apply_operations(window, operations):
    """What a list of literals and matches makes after the window: the reference run of the encoder."""
    buffer = bytearray(window)
    for operation in operations:
        if operation[0] == "literal":
            buffer += operation[1]
        else:
            _, offset, length = operation
            for _ in range(length):
                buffer.append(buffer[len(buffer) - offset])
    return bytes(buffer[len(window):])


def translate_calls(data, translation):
    """The compressor's side of the x86 call translation: relative CALL targets become positions."""
    out = bytearray(data)
    limit = min(len(out), 1 << 30)
    chunk = 0
    while chunk + 10 < limit:
        stop = min(chunk + CHUNK - 10, limit - 10)
        at = chunk
        while at < stop:
            if out[at] == 0xE8:
                relative = struct.unpack_from("<i", out, at + 1)[0]
                if -at <= relative < translation - at:
                    struct.pack_into("<i", out, at + 1, relative + at)
                elif translation - at <= relative < translation:
                    struct.pack_into("<i", out, at + 1, relative - translation)
                at += 4
            at += 1
        chunk += CHUNK
    return bytes(out)


def zeroed(data, ranges):
    out = bytearray(data)
    for offset, length in ranges:
        out[offset:offset + length] = bytes(length)
    return bytes(out)


def patch_file(new, sources, options=LZX_B | NO_REBASE, stamp=None, header_only=False):
    """sources: (old, stream, ignore ranges, retain ranges as (old offset, new offset, length)) for each old file."""
    if stamp is not None:
        options |= NO_TIMESTAMP
    head = bytearray(b"PA19" + struct.pack("<I", options))
    if stamp is not None:
        head += struct.pack("<I", stamp)
    retained_new = [(new_offset, length) for _, new_offset, length in sources[0][3]]
    head += unsigned_number(len(new)) + struct.pack("<I", crc32(zeroed(new, retained_new))) + bytes([len(sources)])
    for old, stream, ignore, retain in sources:
        compared = zeroed(old, [(offset, length) for offset, _, length in retain] + (ignore if stream else []))
        head += signed_number(len(old) - len(new)) + struct.pack("<I", crc32(compared))
        head += bytes([len(ignore)])
        previous = 0
        for offset, length in ignore:
            head += signed_number(offset - previous) + unsigned_number(length)
            previous = offset + length
        head += bytes([len(retain)])
        previous = 0
        for old_offset, new_offset, length in retain:
            head += signed_number(old_offset - previous) + signed_number(new_offset - old_offset) + unsigned_number(length)
            previous = old_offset + length
        head += unsigned_number(0) + unsigned_number(len(stream))
    if not header_only:
        for _, stream, _, _ in sources:
            head += stream
    state = crc32(bytes(head)) ^ 0xFFFFFFFF
    return bytes(head) + struct.pack("<I", state)


def text(seed, size):
    words = [b"patch", b"delta", b"window", b"chunk", b"block", b"literal", b"match", b"raven", b"\r\n", b"0123", b"old", b"new"]
    out, state = bytearray(), seed
    while len(out) < size:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        out += words[state % len(words)] + b" "
    return bytes(out[:size])


def compressed_stream(window, operations, block_types, translation=0):
    """Blocks of the given types over the operations, cut into as many blocks as there are types."""
    operations = split_at_chunks(window, operations)
    size = len(apply_operations(window, operations))
    encoder = Encoder(len(window), size, translation)
    per_block = -(-len(operations) // len(block_types))
    for index, block_type in enumerate(block_types):
        part = operations[index * per_block:(index + 1) * per_block]
        if part:
            encoder.block(block_type, part)
    return encoder.data()


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    old = text(1, 70000)

    # New text made of pieces of the old one, new words and its own repeats, longer than two chunks.
    pieces = [("match", len(old) - 100, 300), ("literal", b"inserted words of the new file\r\n"), ("match", len(old) - 40000 + 330, 3000),
              ("literal", text(7, 5000)), ("match", 6000, 600), ("match", 6000, 40), ("match", len(old) + 9372 - 2000, 20000),
              ("literal", b"tail"), ("match", 4, 9000), ("literal", text(8, 30000))]
    new = apply_operations(old, pieces)
    stream = compressed_stream(old, pieces, [1, 2, 1])
    (out / "old.txt").write_bytes(old)
    (out / "new.txt").write_bytes(new)
    (out / "delta.pa19").write_bytes(patch_file(new, [(old, stream, [], [])], stamp=1700000000))

    # Without an old file: uncompressed runs across chunks, an odd one, then a compressed block.
    plain = text(3, 33001) + text(4, 40001)
    encoder = Encoder(0, len(plain) + 9)
    encoder.raw(plain[:33001], [1, 1, 1])
    encoder.raw(plain[33001:], [5, 1, 1])
    tail = [("literal", b"after"), ("match", 5, 4)]
    encoder.block(1, tail)
    plain += apply_operations(plain, tail)
    (out / "plain.txt").write_bytes(plain)
    (out / "plain.pa19").write_bytes(patch_file(plain, [(b"", encoder.data(), [], [])]))

    # x86 calls: their relative targets were turned into positions before compression.
    calls = bytearray(text(5, 40000))
    for at in range(100, 39000, 997):
        calls[at] = 0xE8
        struct.pack_into("<i", calls, at + 1, (at * 7) % 3000 - 1500)
    calls = bytes(calls)
    translation = 12000000
    encoded = translate_calls(calls, translation)
    stream = compressed_stream(b"", [("literal", encoded)], [1], translation)
    (out / "calls.bin").write_bytes(calls)
    (out / "calls.pa19").write_bytes(patch_file(calls, [(b"", stream, [], [])]))

    # Two old files: each with a range left out of the comparison, both carrying one range over to the new file.
    first = bytearray(text(11, 20000))
    second = bytearray(text(12, 21000))
    first[500:520] = b"FIRST-IGNORED-BYTES!"
    second[900:930] = b"second file ignored bytes here"
    target = bytearray(text(13, 18000))
    target[2000:2064] = bytes(64)
    first[8000:8064] = bytes(range(64))
    second[9000:9064] = bytes(range(64, 128))
    sources = []
    for old_file, ignore, retain in ((bytes(first), [(500, 20)], [(8000, 2000, 64)]), (bytes(second), [(900, 30)], [(9000, 2000, 64)])):
        window = zeroed(old_file, ignore + [(retain[0][0], 64)])
        operations = [("literal", bytes(target[:2000])), ("literal", bytes(64)), ("literal", bytes(target[2064:]))]
        sources.append((old_file, compressed_stream(window, operations, [2]), ignore, retain))
    (out / "first.bin").write_bytes(bytes(first))
    (out / "second.bin").write_bytes(bytes(second))
    target_first = bytearray(target)
    target_first[2000:2064] = first[8000:8064]
    target_second = bytearray(target)
    target_second[2000:2064] = second[9000:9064]
    (out / "target-first.bin").write_bytes(bytes(target_first))
    (out / "target-second.bin").write_bytes(bytes(target_second))
    (out / "two.pa19").write_bytes(patch_file(bytes(target), sources))
    (out / "two-header.pa19").write_bytes(patch_file(bytes(target), sources, header_only=True))

    # Between identical files: no stream at all.
    (out / "same.pa19").write_bytes(patch_file(old, [(old, b"", [], [])]))


if __name__ == "__main__":
    main()
