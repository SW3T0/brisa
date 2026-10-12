from pathlib import Path
root = Path(__file__).parent
def fixture(name, pages, rotation=0, blank=False):
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>', b'']
    kids = []
    font = 3 + pages * 2
    for i in range(pages):
        number = len(objects) + 1
        kids.append(f'{number} 0 R')
        content = (f'0.16 0.25 0.18 rg BT /F1 28 Tf 65 720 Td (A quiet place to study) Tj '
                   f'0 -55 Td /F1 14 Tf (Chapter {i+1} - Read. Think. Make it yours.) Tj '
                   '0 -45 Td (Highlight this line with H. Return to reading with V.) Tj ET '
                   '0.85 0.87 0.81 RG 65 570 m 530 570 l S ')
        for line in range(13):
            content += f'BT /F1 12 Tf 65 {530-line*24} Td (A minimal reader helps you focus on the ideas on the page.) Tj ET '
        raw = b'0.92 0.93 0.9 rg 60 60 450 710 re f' if blank else content.encode('ascii')
        objects.append(f'<< /Type /Page /Parent 2 0 R /Rotate {rotation} /MediaBox [0 0 595 842] /Resources << /Font << /F1 {font} 0 R >> >> /Contents {number+1} 0 R >>'.encode())
        objects.append(f'<< /Length {len(raw)} >>\nstream\n'.encode()+raw+b'\nendstream')
    objects.append(b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>')
    objects[1] = f'<< /Type /Pages /Kids [{" ".join(kids)}] /Count {pages} >>'.encode()
    data = bytearray(b'%PDF-1.4\n')
    offsets = [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data += f'{i} 0 obj\n'.encode() + obj + b'\nendobj\n'
    start = len(data)
    data += f'xref\n0 {len(offsets)}\n0000000000 65535 f \n'.encode()
    for offset in offsets[1:]: data += f'{offset:010} 00000 n \n'.encode()
    data += f'trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{start}\n%%EOF\n'.encode()
    (root / name).write_bytes(data)
fixture('estudio ñ.pdf', 3)
fixture('1000-pages.pdf', 1000)
(root/'broken.pdf').write_text('this is not a PDF')
(root/'empty.pdf').write_bytes(b'')

fixture('rotated.pdf',1,90)
fixture('image-only.pdf',1,blank=True)
