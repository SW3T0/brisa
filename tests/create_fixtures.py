from pathlib import Path

FIXTURE_DIRECTORY = Path(__file__).parent


def create_fixture(name, page_count, rotation=0, blank=False):
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b""]
    page_references = []
    font_object_number = 3 + page_count * 2
    for page_index in range(page_count):
        page_object_number = len(objects) + 1
        page_references.append(f"{page_object_number} 0 R")
        content = (
            "0.16 0.25 0.18 rg BT /F1 28 Tf 65 720 Td (A quiet place to study) Tj "
            f"0 -55 Td /F1 14 Tf (Chapter {page_index+1} - Read. Think. Make it yours.) Tj "
            "0 -45 Td (Highlight this line with H. Return to reading with V.) Tj ET "
            "0.85 0.87 0.81 RG 65 570 m 530 570 l S "
        )
        for line in range(13):
            content += (
                f"BT /F1 12 Tf 65 {530 - line * 24} Td "
                "(A minimal reader helps you focus on the ideas on the page.) Tj ET "
            )
        content_bytes = (
            b"0.92 0.93 0.9 rg 60 60 450 710 re f" if blank else content.encode("ascii")
        )
        objects.append(
            (
                f"<< /Type /Page /Parent 2 0 R /Rotate {rotation} "
                "/MediaBox [0 0 595 842] /Resources << /Font << "
                f"/F1 {font_object_number} 0 R >> >> "
                f"/Contents {page_object_number + 1} 0 R >>"
            ).encode()
        )
        objects.append(
            f"<< /Length {len(content_bytes)} >>\nstream\n".encode()
            + content_bytes
            + b"\nendstream"
        )
    objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    objects[1] = (
        f'<< /Type /Pages /Kids [{" ".join(page_references)}] /Count {page_count} >>'.encode()
    )
    data = bytearray(b"%PDF-1.4\n")
    offsets = [0]
    for object_number, object_bytes in enumerate(objects, 1):
        offsets.append(len(data))
        data += f"{object_number} 0 obj\n".encode() + object_bytes + b"\nendobj\n"
    cross_reference_offset = len(data)
    data += f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode()
    for offset in offsets[1:]:
        data += f"{offset:010} 00000 n \n".encode()
    data += (
        f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\n"
        f"startxref\n{cross_reference_offset}\n%%EOF\n"
    ).encode()
    (FIXTURE_DIRECTORY / name).write_bytes(data)


def main():
    create_fixture("estudio ñ.pdf", 3)
    create_fixture("1000-pages.pdf", 1000)
    (FIXTURE_DIRECTORY / "broken.pdf").write_text("this is not a PDF")
    (FIXTURE_DIRECTORY / "empty.pdf").write_bytes(b"")
    create_fixture("rotated.pdf", 1, 90)
    create_fixture("image-only.pdf", 1, blank=True)


if __name__ == "__main__":
    main()
