"""Run against a BRISA_TEST build; requires PyMuPDF (test-only dependency)."""
import hashlib
import struct
import subprocess
import sys
from pathlib import Path
import pymupdf as pdf

exe = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2]).resolve()
root.mkdir(parents=True, exist_ok=True)

def run(source, target, success=True):
    before = hashlib.sha256(source.read_bytes()).digest()
    result = subprocess.run([str(exe), '--export-test', str(source), str(target)])
    assert (result.returncode == 0) == success
    assert hashlib.sha256(source.read_bytes()).digest() == before

for rotation in (0, 90, 180, 270):
    source = root / f'original-{rotation}.pdf'
    doc = pdf.open()
    page = doc.new_page(width=595, height=842)
    page.insert_text((70, 90), 'A quiet place to study', fontsize=22)
    page.set_rotation(rotation)
    page.add_text_annot((300, 300), 'Existing annotation')
    doc.new_page()
    doc.save(source)
    doc.close()
    # BRISA02 records use Win32 FILETIME and the original file's size.
    stamp = source.stat().st_mtime_ns // 100 + 116444736000000000
    header = struct.pack('<8sIIIII', b'BRISA02\0', 3, source.stat().st_size, 0, stamp & 0xffffffff, stamp >> 32)
    ink = b''.join(struct.pack('<iiiff', 0, int(n == 0), 2, x, y)
                   for n, (x, y) in enumerate(((.2, .4), (.4, .5), (.6, .4))))
    highlight = struct.pack('<iiffff', 0, 1, .12, .10, .48, .14)
    Path(str(source) + '.brisa-ink').write_bytes(header + ink + struct.pack('<I', 1) + highlight)
    target = root / f'annotated-{rotation}.pdf'
    run(source, target)
    original, saved = pdf.open(source), pdf.open(target)
    assert len(saved) == 2
    saved_page = saved[0]
    annotations = list(saved_page.annots())
    assert sorted(a.type[1] for a in annotations) == ['Highlight', 'Ink', 'Text']
    # An independent renderer must actually display both new marks.
    a = original[0].get_pixmap().samples
    b = saved_page.get_pixmap().samples
    assert sum(x != y for x, y in zip(a, b)) > 1000
    saved_page.get_pixmap().save(root / f'export-{rotation}.png')
    original.close(); saved.close()
    run(source, source, False)
    run(source, root / 'missing-directory' / 'output.pdf', False)
    # Existing destinations must survive failed exports.
    bad = root / 'broken.pdf'; bad.write_bytes(b'not a pdf')
    before = target.read_bytes()
    run(bad, target, False)
    assert target.read_bytes() == before
    run(source, target)
    assert not list(root.glob('*.tmp'))
print('PASS: embedded highlight/ink, rotations, existing annotations, all pages, original preservation, overwrite and failure safety')
