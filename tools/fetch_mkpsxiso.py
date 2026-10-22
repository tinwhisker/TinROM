#!/usr/bin/env python3
# tinrom - (C) 2026 TinWhisker - MIT licensed
"""Fetches mkpsxiso, which builds the boot disc, into tools/bin/.

    python3 tools/fetch_mkpsxiso.py [--force]

Run by scripts/bootstrap.sh and scripts/bootstrap.ps1. mkpsxiso is Lameguy64's
(https://github.com/Lameguy64/mkpsxiso), under the GPL-2.0, so tinrom does
not carry it: this downloads the release named below, for this platform,
from the project's own GitHub releases, checks it against the SHA-256 noted
here when it was pinned, and unpacks mkpsxiso - with whatever sits beside it
in the zip, dumpsxiso included - into tools/bin/, which git ignores.
cmake/iso.cmake looks there before PATH. See NOTICE.md.

Already there at this version: nothing to do, unless --force.

Only the boot disc target needs it. If the download fails, the rest of the
build is unaffected - cmake skips that one target and says so.

Standard library only, like the rest of the build's tools.
"""

import argparse
import hashlib
import io
import os
import stat
import sys
import urllib.request
import zipfile

VERSION = "2.30"
URL     = ("https://github.com/Lameguy64/mkpsxiso/releases/download/"
           "v{version}/mkpsxiso-{version}-{platform}.zip")
TOOLS   = ("mkpsxiso", "dumpsxiso")

# The release's zips as they were when this version was pinned. A download
# that does not match is refused, not run: a release asset can be replaced.
# Moving to another version means new sums here, from the printed output.
SHA256 = {
    "win64":  "7d3963cd322df365766cee1dbc3e405ea103bbe4e11c0db86c9688b32f5a1017",
    "Linux":  "8e387e6db51dee3eb7cb08f4d59293a24c48f58bd541631acf9301d78015a1fc",
    "Darwin": "a64d4d57d78ce57cd810a6a044602ce3cf1b552ca7c7e8675518f554206e0b49",
}

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN   = os.path.join(ROOT, "tools", "bin")
STAMP = os.path.join(BIN, "mkpsxiso.version")


def die(message):
    print(f"fetch_mkpsxiso: {message}", file=sys.stderr)
    sys.exit(1)


def release_platform():
    """The release's name for this machine. An MSYS2 or Cygwin Python on
    Windows still wants the Windows build: that is what cmake will run."""
    if sys.platform in ("win32", "msys", "cygwin"):
        return "win64"
    if sys.platform.startswith("linux"):
        return "Linux"
    if sys.platform == "darwin":
        return "Darwin"

    die(f"no mkpsxiso release for {sys.platform}; install it yourself and "
        "put it on PATH")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--force", action="store_true",
        help="download again even if this version is already there")
    args = parser.parse_args()

    platform = release_platform()
    exe      = ".exe" if platform == "win64" else ""

    try:
        with open(STAMP) as file:
            have = file.read().strip()
    except OSError:
        have = ""

    if (have == f"{VERSION} {platform}") and not args.force and os.path.exists(
            os.path.join(BIN, "mkpsxiso" + exe)):
        print(f"mkpsxiso {VERSION} is in place.")
        return

    url = URL.format(version=VERSION, platform=platform)
    print(f"Downloading mkpsxiso {VERSION} for {platform}...")

    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read()
    except OSError as error:
        die(f"could not download {url}: {error}")

    digest = hashlib.sha256(data).hexdigest()

    print(f"  {len(data)} bytes, sha256 {digest}")

    if digest != SHA256[platform]:
        die(f"{url} is not the file this version was pinned to (expected "
            f"sha256 {SHA256[platform]}); refusing to use it")

    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile:
        die(f"{url} is not a zip file")

    # Wherever the release keeps it inside the zip: found by name, and then
    # everything beside it comes too, in case a build ships libraries.
    home = None

    for member in archive.infolist():
        if os.path.basename(member.filename) == "mkpsxiso" + exe:
            home = os.path.dirname(member.filename)
            break

    if home is None:
        die(f"{url} has no mkpsxiso{exe} in it")

    os.makedirs(BIN, exist_ok=True)
    found = []

    for member in archive.infolist():
        if member.is_dir() or os.path.dirname(member.filename) != home:
            continue

        name   = os.path.basename(member.filename)
        target = os.path.join(BIN, name)

        with archive.open(member) as source, open(target, "wb") as out:
            out.write(source.read())

        if not exe and name in TOOLS:
            mode = os.stat(target).st_mode
            os.chmod(target, mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

        found.append(name)

    with open(STAMP, "w") as file:
        file.write(f"{VERSION} {platform}\n")

    print(f"  {', '.join(sorted(found))} -> tools/bin/")


if __name__ == "__main__":
    main()
