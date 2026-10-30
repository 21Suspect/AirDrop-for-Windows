# SPDX-License-Identifier: GPL-3.0-or-later
import errno
import os
import time
from pathlib import Path

import pytest

from opendrop.windows.organize import category, publish_files, readable_name


def test_flat_folders_readable_names_and_duplicates(tmp_path):
    staging, output = tmp_path / "staging", tmp_path / "AirDrop"
    staging.mkdir()
    stamp = 1793394000
    files = ["IMG_1234.JPG", "B6A8D31C-7D88-4314-9F58-F6772F95A1E8.MP4", "invoice.pdf", "nested/note.txt"]
    for i, name in enumerate(files):
        path = staging / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(bytes([i]) * 1000)
        os.utime(path, (stamp, stamp))
    _, names = publish_files(staging, output, files)
    assert [name.split("/")[0] for name in names] == ["Pictures", "Videos", "Files", "Files"]
    assert all(len(name.split("/")) == 2 for name in names)
    assert names[0].startswith("Pictures/Photo ")
    assert names[1].startswith("Videos/Video ") and names[1].endswith(".mp4")
    assert names[2] == "Files/invoice.pdf"
    assert names[3] == "Files/note.txt"
    original = [(output / name).read_bytes() for name in names]
    _, duplicates = publish_files(staging, output, files)
    assert all(" (2)." in name for name in duplicates)
    assert [(output / name).read_bytes() for name in names] == original
    assert [(output / name).read_bytes() for name in duplicates] == original


def test_organize_rolls_back_published_files_on_error(tmp_path, monkeypatch):
    from opendrop.windows import organize
    staging = tmp_path / "stage"
    staging.mkdir()
    for name in ("one.txt", "two.txt"):
        (staging / name).write_text(name)
    real = organize._publish
    def fail_second(source, destination):
        if source.name == "two.txt":
            raise OSError("disk full")
        real(source, destination)
    monkeypatch.setattr(organize, "_publish", fail_second)
    with pytest.raises(OSError, match="disk full"):
        publish_files(staging, tmp_path / "output", ["one.txt", "two.txt"])
    assert not list((tmp_path / "output").rglob("*.txt"))
    assert (staging / "one.txt").read_text() == "one.txt"


@pytest.mark.skipif(os.name != "nt", reason="Windows no-replace cross-volume fallback")
def test_cross_volume_copy_preserves_content_and_does_not_overwrite(tmp_path, monkeypatch):
    from opendrop.windows import organize
    staging, output = tmp_path / "stage", tmp_path / "output"
    staging.mkdir()
    (staging / "report.txt").write_text("new content")
    (output / "Files").mkdir(parents=True)
    (output / "Files/report.txt").write_text("existing content")
    def cross_volume(*args, **kwargs):
        raise OSError(errno.EXDEV, "cross-volume")
    monkeypatch.setattr(organize.os, "link", cross_volume)
    _, names = publish_files(staging, output, ["report.txt"])
    assert names == ["Files/report (2).txt"]
    assert (output / "Files/report.txt").read_text() == "existing content"
    assert (output / names[0]).read_text() == "new content"
    assert not list((output / "Files").glob("*.partial"))
    assert "ZoneId=3" in Path(str(output / names[0]) + ":Zone.Identifier").read_text()


def test_names_are_classified_without_touching_file_contents():
    assert category("document.pdf") == "Files"
    assert category("clip.MOV") == "Videos"
    assert category("image.HEIC") == "Pictures"
    assert readable_name("My holiday.JPG", time.time()) == "My holiday.jpg"
