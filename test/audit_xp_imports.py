#!/usr/bin/env python3
"""Audit every packaged PE's imports and subsystem against Windows XP exports."""
import argparse
import pathlib
import re
import subprocess
import sys


def objdump(tool, path):
    result = subprocess.run([tool, "-p", str(path)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(f"{tool} failed for {path}: {result.stderr.strip()}")
    return result.stdout


def architecture(tool, path):
    output = subprocess.run([tool, "-f", str(path)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if output.returncode:
        raise RuntimeError(f"{tool} failed for {path}: {output.stderr.strip()}")
    if "pei-x86-64" in output.stdout:
        return "x64"
    if "pei-i386" in output.stdout:
        return "x86"
    return None


def exported_names(tool, path):
    output = objdump(tool, path)
    table = output.split("[Ordinal/Name Pointer] Table", 1)
    if len(table) != 2:
        return set()
    names = set()
    for line in table[1].splitlines():
        match = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s*$", line)
        if match:
            names.add(match.group(1))
    return names


def imported_names(tool, path):
    output = objdump(tool, path)
    imports = {}
    current = None
    for line in output.splitlines():
        match = re.search(r"DLL Name: (\S+)", line)
        if match:
            current = match.group(1).lower()
            imports.setdefault(current, set())
            continue
        if current:
            match = re.match(r"\s*[0-9a-fA-F]{6,}\s+\d+\s+([^\s]+)\s*$", line)
            if match:
                imports[current].add(match.group(1))
    return imports


def subsystem_version(tool, path):
    output = objdump(tool, path)
    major = re.search(r"^MajorSubsystemVersion\s+(\d+)", output, re.MULTILINE)
    minor = re.search(r"^MinorSubsystemVersion\s+(\d+)", output, re.MULTILINE)
    if not major or not minor:
        return None
    return int(major.group(1)), int(minor.group(1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("system_dirs", nargs="*", type=pathlib.Path,
                        help="XP x64 system32 and system32/drivers directories")
    parser.add_argument("--x64-system-dir", action="append", type=pathlib.Path,
                        default=[], help="additional XP x64 system directory")
    parser.add_argument("--x86-system-dir", action="append", type=pathlib.Path,
                        default=[], help="XP x86 system directory (repeatable)")
    parser.add_argument("--objdump-x64", default="x86_64-w64-mingw32-objdump")
    parser.add_argument("--objdump-x86", default="i686-w64-mingw32-objdump")
    parser.add_argument("--package", type=pathlib.Path, action="append",
                        required=True, help="package directory or individual PE; repeatable")
    args = parser.parse_args()

    tools = {"x64": args.objdump_x64, "x86": args.objdump_x86}
    directories = {"x64": args.system_dirs + args.x64_system_dir,
                   "x86": args.x86_system_dir}
    exports = {"x64": {}, "x86": {}}
    for arch, arch_dirs in directories.items():
        for directory in arch_dirs:
            for path in directory.iterdir():
                if path.suffix.lower() not in {".dll", ".sys", ".exe"}:
                    continue
                try:
                    names = exported_names(tools[arch], path)
                except RuntimeError:
                    continue
                exports[arch][path.name.lower()] = names
    if args.x86_system_dir and not exports["x86"]:
        parser.error("no XP x86 DLL/SYS export tables found in --x86-system-dir")

    failed = False
    package_files = set()
    for package in args.package:
        if package.is_dir():
            package_files.update(path for path in package.rglob("*") if path.is_file())
        elif package.is_file():
            package_files.add(package)
        else:
            parser.error(f"package path does not exist: {package}")
    package_files = sorted(package_files)
    # DLLs shipped together may import one another (for example the test-only
    # Mesa WGL frontend imports nvxogl64.dll). Include package exports in the
    # same resolution set as the XP system DLLs.
    for path in package_files:
        if not path.is_file() or path.suffix.lower() not in {".dll", ".sys", ".exe"}:
            continue
        try:
            arch = architecture(args.objdump_x64, path)
            if arch:
                exports[arch][path.name.lower()] = exported_names(tools[arch], path)
        except RuntimeError as error:
            print(f"XP PE AUDIT FAIL {path.name}: {error}")
            failed = True
    for path in package_files:
        if not path.is_file() or path.suffix.lower() not in {".dll", ".sys", ".exe"}:
            continue
        try:
            arch = architecture(args.objdump_x64, path)
        except RuntimeError as error:
            failed = True
            print(f"XP PE AUDIT FAIL {path.name}: {error}")
            continue
        if arch is None:
            continue
        tool = tools[arch]
        if not exports[arch]:
            failed = True
            print(f"XP IMPORT AUDIT FAIL {path.name}: no XP {arch} export set supplied")
            continue
        try:
            imports = imported_names(tool, path)
            version = subsystem_version(tool, path)
        except RuntimeError as error:
            failed = True
            print(f"XP PE AUDIT FAIL {path.name}: {error}")
            continue
        if version is None or version > (5, 2):
            failed = True
            actual = f"{version[0]}.{version[1]}" if version else "unavailable"
            print(f"XP SUBSYSTEM AUDIT FAIL {path.name}: {actual} (XP maximum is 5.2)")
        missing = []
        for module, names in imports.items():
            available = exports[arch].get(module)
            if available is None:
                missing.append((module, "<module missing>"))
            else:
                missing.extend((module, name) for name in sorted(names - available))
        if missing:
            failed = True
            details = ", ".join(f"{module}!{name}" for module, name in missing)
            print(f"XP IMPORT AUDIT FAIL {path.name}: {details}")
        elif imports:
            actual = f"{version[0]}.{version[1]}" if version else "unavailable"
            print(f"XP IMPORT/SUBSYSTEM AUDIT PASS {path.name} (subsystem {actual})")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
