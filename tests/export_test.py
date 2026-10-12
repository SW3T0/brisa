"""Run against a BRISA_TEST build; requires PyMuPDF (test-only dependency)."""

import hashlib
import struct
import subprocess
import sys
from pathlib import Path

import pymupdf as pdf

WINDOWS_EPOCH_OFFSET = 116444736000000000
PAGE_ROTATIONS = (0, 90, 180, 270)


def run_export(executable, source, target, expect_success=True):
    original_hash = hashlib.sha256(source.read_bytes()).digest()
    result = subprocess.run(
        [str(executable), "--export-test", str(source), str(target)]
    )
    assert (result.returncode == 0) == expect_success
    assert hashlib.sha256(source.read_bytes()).digest() == original_hash


def create_original_pdf(source, rotation):
    document = pdf.open()
    page = document.new_page(width=595, height=842)
    page.insert_text((70, 90), "A quiet place to study", fontsize=22)
    page.set_rotation(rotation)
    page.add_text_annot((300, 300), "Existing annotation")
    document.new_page()
    document.save(source)
    document.close()


def write_brisa_marks(source):
    # BRISA02 records use Win32 FILETIME and the original file's size.
    file_time = source.stat().st_mtime_ns // 100 + WINDOWS_EPOCH_OFFSET
    header = struct.pack(
        "<8sIIIII",
        b"BRISA02\0",
        3,
        source.stat().st_size,
        0,
        file_time & 0xFFFFFFFF,
        file_time >> 32,
    )
    stroke_points = ((0.2, 0.4), (0.4, 0.5), (0.6, 0.4))
    ink = b"".join(
        struct.pack("<iiiff", 0, int(index == 0), 2, x, y)
        for index, (x, y) in enumerate(stroke_points)
    )
    highlight = struct.pack("<iiffff", 0, 1, 0.12, 0.10, 0.48, 0.14)
    sidecar = Path(str(source) + ".brisa-ink")
    sidecar.write_bytes(header + ink + struct.pack("<I", 1) + highlight)


def verify_rotation(executable, root, rotation):
    source = root / f"original-{rotation}.pdf"
    create_original_pdf(source, rotation)
    write_brisa_marks(source)
    target = root / f"annotated-{rotation}.pdf"
    run_export(executable, source, target)
    original, saved = pdf.open(source), pdf.open(target)
    assert len(saved) == 2
    saved_page = saved[0]
    annotations = list(saved_page.annots())
    assert sorted(annotation.type[1] for annotation in annotations) == [
        "Highlight",
        "Ink",
        "Text",
    ]
    # An independent renderer must actually display both new marks.
    original_pixels = original[0].get_pixmap().samples
    exported_pixels = saved_page.get_pixmap().samples
    assert (
        sum(before != after for before, after in zip(original_pixels, exported_pixels))
        > 1000
    )
    saved_page.get_pixmap().save(root / f"export-{rotation}.png")
    original.close()
    saved.close()
    run_export(executable, source, source, expect_success=False)
    run_export(
        executable,
        source,
        root / "missing-directory" / "output.pdf",
        expect_success=False,
    )
    # Existing destinations must survive failed exports.
    broken_pdf = root / "broken.pdf"
    broken_pdf.write_bytes(b"not a pdf")
    existing_output = target.read_bytes()
    run_export(executable, broken_pdf, target, expect_success=False)
    assert target.read_bytes() == existing_output
    run_export(executable, source, target)
    assert not list(root.glob("*.tmp"))


def main():
    executable = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    root.mkdir(parents=True, exist_ok=True)
    for rotation in PAGE_ROTATIONS:
        verify_rotation(executable, root, rotation)
    print(
        "PASS: embedded highlight/ink, rotations, existing annotations, all pages, "
        "original preservation, overwrite and failure safety"
    )


if __name__ == "__main__":
    main()
