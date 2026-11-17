#!/usr/bin/env python3
"""Turns the benchmark's profile into time per function.

    pico/profile.py <monitor log> <elf> [functions] [profile]

Reads the first complete profile from the log of pico/pico.sh monitor, decode by default or screen, and maps
each sampled address to the function holding it, using arm-none-eabi-nm. Prints the busiest functions,
default 30.
"""
import bisect
import subprocess
import sys


def symbols(elf):
    out = subprocess.run(['arm-none-eabi-nm', '-n', '-C', '--defined-only', elf], capture_output=True, text=True,
                         check=True).stdout
    starts, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'TtWw':
            starts.append(int(parts[0], 16) & ~1)
            names.append(parts[2])
    return starts, names


def main():
    log, elf = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    wanted = sys.argv[4] if len(sys.argv) > 4 else 'decode'
    samples, header, inside = {}, '', False
    for line in open(log, errors='replace'):
        line = line.strip()
        if line.startswith('profile: ' + wanted + ','):
            if samples:
                break
            header, inside = line, True
        elif line.startswith('profile:'):
            inside = False
        elif inside and line.startswith('P '):
            _, address, count = line.split()
            samples[int(address, 16)] = int(count)
        elif inside and line == 'profile end':
            break
    if not samples:
        sys.exit('No profile in ' + log)
    starts, names = symbols(elf)
    totals = {}
    for address, count in samples.items():
        i = bisect.bisect_right(starts, address) - 1
        name = names[i] if i >= 0 else '?'
        totals[name] = totals.get(name, 0) + count
    total = sum(totals.values())
    print(header)
    for name, count in sorted(totals.items(), key=lambda item: -item[1])[:top]:
        print(f'{100.0 * count / total:6.2f}%  {count:7d}  {name}')


main()
