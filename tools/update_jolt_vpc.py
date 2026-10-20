#!/usr/bin/env python3
"""Regenerates the Jolt file list in joltphysics/joltphysics_inc.vpc from Jolt's Jolt.cmake.

Run after updating the joltphysics/src submodule. (The CMake build reads Jolt.cmake directly.)
"""
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
cmake = (root / "joltphysics/src/Jolt/Jolt.cmake").read_text()
vpc_path = root / "joltphysics/joltphysics_inc.vpc"
vpc = vpc_path.read_text()

block = re.search(r"set\(JOLT_PHYSICS_SRC_FILES(.*?)\)", cmake, re.S)
if not block:
    raise SystemExit("JOLT_PHYSICS_SRC_FILES not found in Jolt.cmake")
files = re.findall(r"\$\{JOLT_PHYSICS_ROOT\}/(\S+)", block.group(1))

lines = "".join(f'\t\t$File\t"$JOLT_PHYSICS_ROOT/{f}"\n' for f in files)
# The "Source Files" folder is the last thing in the project
start = vpc.find('\t$Folder "Source Files"')
if start < 0:
    raise SystemExit('"Source Files" folder not found in joltphysics_inc.vpc')
new_vpc = vpc[:start] + '\t$Folder "Source Files"\n\t{\n' + lines + "\t}\n}\n"

vpc_path.write_text(new_vpc)
print(f"{len(files)} files")
