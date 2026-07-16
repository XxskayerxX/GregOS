#!/usr/bin/env python3
"""Generate a C header embedding a binary file as a static unsigned char array."""

import sys
import os

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.elf> <output.h>", file=sys.stderr)
        sys.exit(1)

    in_path  = sys.argv[1]
    out_path = sys.argv[2]

    with open(in_path, "rb") as f:
        data = f.read()

    # Derive symbol name from basename, replacing non-alphanum with _
    base = os.path.basename(in_path)
    sym  = "".join(c if c.isalnum() else "_" for c in base)
    guard = sym.upper() + "_DATA_H"

    with open(out_path, "w") as f:
        f.write(f"#ifndef {guard}\n")
        f.write(f"#define {guard}\n")
        f.write(f"/* Auto-generated — {base} embedded as kernel built-in ELF */\n")
        f.write(f"static const unsigned char {sym}[] = {{\n")

        cols = 12
        for i, byte in enumerate(data):
            if i % cols == 0:
                f.write("    ")
            f.write(f"0x{byte:02x}u,")
            if (i + 1) % cols == 0:
                f.write("\n")
            else:
                f.write(" ")
        if len(data) % cols != 0:
            f.write("\n")

        f.write(f"}};\n")
        f.write(f"static const unsigned int {sym}_len = {len(data)}u;\n")
        f.write(f"#endif /* {guard} */\n")

    print(f"[make_elf_header] {in_path} ({len(data)} bytes) -> {out_path} ({sym})")

if __name__ == "__main__":
    main()
