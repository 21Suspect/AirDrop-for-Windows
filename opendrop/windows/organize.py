# SPDX-License-Identifier: GPL-3.0-or-later
"""Flat, collision-safe placement of already validated incoming files."""
import errno
import os
import re
import shutil
import stat
import tempfile
from datetime import datetime
from pathlib import Path

PICTURES = {".jpg", ".jpeg", ".png", ".heic", ".heif", ".gif", ".webp", ".bmp",
            ".tif", ".tiff", ".avif", ".dng"}
VIDEOS = {".mp4", ".mov", ".m4v", ".avi", ".mkv", ".webm", ".wmv", ".mpeg",
          ".mpg", ".3gp", ".mts", ".m2ts"}
GENERATED = re.compile(
    r"^(?:[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}|[0-9a-f]{24,64}|"
    r"(?:IMG|DSC|DSCF|DSCN|VID|MOV)[_-]?[A-Z]?\d+)$", re.IGNORECASE)


def category(name):
    suffix = Path(name).suffix.lower()
    return "Pictures" if suffix in PICTURES else "Videos" if suffix in VIDEOS else "Files"


def readable_name(name, timestamp):
    path = Path(name)
    suffix = path.suffix.lower()
    stem = path.stem.strip("{}")
    if GENERATED.fullmatch(stem):
        prefix = {"Pictures": "Photo", "Videos": "Video", "Files": "File"}[category(name)]
        stamp = datetime.fromtimestamp(timestamp).strftime("%Y-%m-%d %H.%M.%S")
        return f"{prefix} {stamp}{suffix}"
    return path.stem + suffix


def plain_directory(path):
    """Do not follow a junction introduced inside the selected receive folder."""
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise ValueError("A receive category must be a regular folder, not a link or junction")
    return path


def _hidden(path, hidden):
    if os.name == "nt":
        import ctypes
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.SetFileAttributesW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
        api.SetFileAttributesW.restype = ctypes.c_int
        if not api.SetFileAttributesW(str(path), 2 if hidden else 0x80):
            raise ctypes.WinError(ctypes.get_last_error())


def _publish(source, destination):
    try:
        # A hard link publishes an entire validated file and never overwrites.
        # The staging link is removed only after the complete batch succeeds.
        os.link(source, destination)
        return
    except FileExistsError:
        raise
    except OSError as exc:
        if exc.errno not in (errno.EXDEV, errno.EPERM, errno.EACCES, errno.ENOTSUP,
                             errno.EOPNOTSUPP, errno.EINVAL) and getattr(exc, "winerror", 0) not in (1, 17, 50, 1314):
            raise
    # Cross-volume/FAT destination: copy to a temporary sibling, then publish
    # with Windows' no-replace rename. Never expose a partly copied final file.
    fd, temporary = tempfile.mkstemp(prefix=".airdrop-", suffix=".partial", dir=destination.parent)
    temporary = Path(temporary)
    try:
        with os.fdopen(fd, "wb") as target, source.open("rb") as incoming:
            _hidden(temporary, True)
            shutil.copyfileobj(incoming, target, 1024 * 1024)
            target.flush()
            os.fsync(target.fileno())
        shutil.copystat(source, temporary)
        if os.name == "nt":
            with open(str(temporary) + ":Zone.Identifier", "w", encoding="ascii") as zone:
                zone.write("[ZoneTransfer]"+chr(10)+"ZoneId=3"+chr(10))
            _hidden(temporary, False)
            os.rename(temporary, destination)  # Windows refuses an existing target.
        else:
            os.link(temporary, destination)
            temporary.unlink()
    finally:
        if temporary.exists():
            temporary.unlink()


def publish_files(staging, output, files):
    """Publish flat names only after archive validation; roll back on failure."""
    staging = Path(staging).resolve(strict=True)
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    created, names = [], []
    try:
        for relative in files:
            source = staging.joinpath(*Path(relative).parts)
            resolved = source.resolve(strict=True)
            if not resolved.is_relative_to(staging) or not stat.S_ISREG(source.lstat().st_mode):
                raise ValueError("Only validated regular staged files can be published")
            folder = plain_directory(output / category(source.name))
            name = readable_name(source.name, source.stat().st_mtime)
            stem, suffix = Path(name).stem, Path(name).suffix
            for index in range(1, 10001):
                destination = folder / (name if index == 1 else f"{stem} ({index}){suffix}")
                try:
                    _publish(source, destination)
                    break
                except FileExistsError:
                    continue
            else:
                raise FileExistsError("Too many files with this name")
            info = destination.stat()
            created.append((destination, info.st_dev, info.st_ino))
            names.append(destination.relative_to(output).as_posix())
        return output, names
    except BaseException:
        for destination, dev, ino in reversed(created):
            try:
                current = destination.lstat()
                if (current.st_dev, current.st_ino) == (dev, ino):
                    destination.unlink()
            except FileNotFoundError:
                pass
        raise
