#!/usr/bin/env python3
"""Reports a profile the Knulli port recorded (HALO_PROFILE_HZ,
port/knulli/host/host_profile.c): for each busy thread, the functions its
samples were in (self) and the functions on their stacks (inclusive).

    profile.py <profile.txt> --guest build/knulli/halo_guest.elf
               --lib <folder with the device's libraries and the host>
               [--top 40] [--thread <tid>] [--frames-over <ms>]

--frames-over keeps only the samples taken during the game thread's frames
longer than that: the host marks the end of each of its frames among the
samples (host_profile_mark, written as thread -1), so that a hitch's work
can be told from a smooth frame's.

Addresses in the guest image are symbolized with llvm-symbolizer against
the guest ELF; addresses in the host and the libraries against the files of
the same names in --lib (their dynamic symbols when they are stripped).
"""

import argparse
import collections
import os
import re
import shutil
import subprocess
import sys


def llvm_tool(tool):
    """an LLVM tool by its versioned name or its plain one, or None"""
    return next((name for name in (f"{tool}-22", tool) if shutil.which(name)), None)


def symbolizer():
    return llvm_tool("llvm-symbolizer") or sys.exit("llvm-symbolizer not found")


def parse(path):
    maps, threads, samples = [], {}, []
    section = None
    with open(path, encoding="utf-8", errors="replace") as profile:
        for line in profile:
            line = line.rstrip("\n")
            if line.startswith("# maps"):
                section = "maps"
                continue
            if line.startswith("# threads"):
                section = "threads"
                continue
            if line.startswith("# samples"):
                section = "samples"
                continue
            if section == "maps":
                found = re.match(r"([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+\s*(.*)", line)
                if found:
                    maps.append((int(found.group(1), 16), int(found.group(2), 16), found.group(3),
                                 int(found.group(4), 16), found.group(5)))
            elif section == "threads":
                parts = line.split()
                if len(parts) >= 5:
                    threads[int(parts[1])] = (parts[2], int(parts[3]), int(parts[4]))
            elif section == "samples":
                parts = line.split()
                if len(parts) >= 3:
                    samples.append((int(parts[0]), [int(value, 16) for value in parts[1:]]))
    return maps, threads, samples


class Symbols:
    def __init__(self, maps, guest, library_folder):
        self.maps = maps
        self.guest = guest
        self.library_folder = library_folder
        self.cache = {}
        self.guest_low, self.guest_high = 0x88000000, 0x89000000

    def locate(self, address):
        """(file to symbolize with, address in it) or None"""
        if self.guest_low <= address < self.guest_high:
            return self.guest, address
        for low, high, permissions, offset, path in self.maps:
            if low <= address < high and path.startswith("/"):
                local = os.path.join(self.library_folder, os.path.basename(path))
                if not os.path.exists(local):
                    return None
                # the file's first mapping gives the load base
                base = min(m[0] - m[3] for m in self.maps if m[4] == path)
                return local, address - base
        return None

    def resolve(self, addresses):
        pending = collections.defaultdict(list)
        for address in set(addresses):
            if address in self.cache:
                continue
            place = self.locate(address)
            if not place:
                self.cache[address] = self.region(address)
                continue
            pending[place[0]].append((address, place[1]))
        tool = symbolizer()
        for path, entries in pending.items():
            text = "\n".join(f"0x{relative:x}" for _, relative in entries) + "\n"
            result = subprocess.run([tool, f"--obj={path}", "--output-style=GNU", "--no-inlines"],
                                    input=text, capture_output=True, text=True)
            lines = result.stdout.split("\n")
            # GNU style: function, then file:line, per address
            names = lines[0::2]
            table = None
            for (address, relative), name in zip(entries, names):
                name = name.strip() or "??"
                if name == "??":
                    # a stripped library: the nearest exported symbol below
                    if table is None:
                        table = self.exported(path)
                    name = self.nearest(table, relative) or f"0x{relative:x}"
                if path != self.guest:
                    name = f"{name} [{os.path.basename(path)}]"
                self.cache[address] = name

    @staticmethod
    def exported(path):
        nm = llvm_tool("llvm-nm")
        if not nm:
            return []
        result = subprocess.run([nm, "-D", "--defined-only", path], capture_output=True, text=True)
        table = []
        for line in result.stdout.split("\n"):
            parts = line.split()
            if len(parts) == 3 and parts[1] in "TtWi":
                table.append((int(parts[0], 16), parts[2]))
        table.sort()
        return table

    @staticmethod
    def nearest(table, address):
        low, high = 0, len(table)
        while low < high:
            middle = (low + high) // 2
            if table[middle][0] <= address:
                low = middle + 1
            else:
                high = middle
        if not low:
            return None
        start, name = table[low - 1]
        return f"{name}+0x{address - start:x}"

    def region(self, address):
        for low, high, _, _, path in self.maps:
            if low <= address < high:
                return f"[{os.path.basename(path) or 'anonymous'}]"
        return f"0x{address:x}"

    def name(self, address):
        return self.cache.get(address, f"0x{address:x}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("profile")
    parser.add_argument("--guest", required=True)
    parser.add_argument("--lib", required=True)
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--thread", type=int)
    parser.add_argument("--frames-over", type=float, metavar="MS")
    arguments = parser.parse_args()
    maps, threads, samples = parse(arguments.profile)
    if arguments.frames_over is not None:
        # the samples between a frame's mark and the one before, for the long frames
        kept, start, frames, milliseconds = [], 0, 0, 0.0
        for index, (tid, stack) in enumerate(samples):
            if tid != -1:
                continue
            if stack[1] / 1000.0 > arguments.frames_over:
                kept.extend(samples[start:index])
                frames += 1
                milliseconds += stack[1] / 1000.0
            start = index + 1
        print(f"{frames} frames over {arguments.frames_over:g} ms, {milliseconds:.0f} ms in all")
        samples = kept
    samples = [sample for sample in samples if sample[0] != -1]
    by_thread = collections.Counter(tid for tid, _ in samples)
    print(f"{len(samples)} samples")
    print("thread           samples   cpu ticks (user+system)")
    for tid, count in by_thread.most_common():
        name, user, system = threads.get(tid, ("?", 0, 0))
        print(f"  {tid:>6} {name:<12} {count:>7}   {user}+{system}")
    symbols = Symbols(maps, arguments.guest, arguments.lib)
    chosen = [arguments.thread] if arguments.thread else [tid for tid, count in by_thread.most_common(4)]
    everything = [address for tid, stack in samples if tid in chosen for address in stack]
    symbols.resolve(everything)
    for tid in chosen:
        stacks = [stack for sample_tid, stack in samples if sample_tid == tid]
        total = len(stacks) or 1
        own = collections.Counter(symbols.name(stack[0]) for stack in stacks)
        inclusive = collections.Counter()
        for stack in stacks:
            # pc, lr, then the frame records: each function once per sample
            inclusive.update(set(symbols.name(address) for address in [stack[0]] + stack[2:]))
        print(f"\n== thread {tid} ({threads.get(tid, ('?',))[0]}): {len(stacks)} samples")
        print("-- self")
        for name, count in own.most_common(arguments.top):
            print(f"  {100.0 * count / total:5.1f}%  {name}")
        print("-- inclusive")
        for name, count in inclusive.most_common(arguments.top):
            print(f"  {100.0 * count / total:5.1f}%  {name}")


if __name__ == "__main__":
    main()
