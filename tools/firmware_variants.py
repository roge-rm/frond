"""Match generated firmware variants using the core's first Palm card header."""
import struct


def firmware_variant(nor_path, profile):
    """Return a fully matched firmware_variants entry, or None. Read only headers."""
    variants = profile.get('firmware_variants', [])
    if not variants:
        return None
    with open(nor_path, 'rb') as nor:
        nor.seek(0, 2)
        size = nor.tell()
        candidates = [v for v in variants if v['nor_bytes'] == size]
        if not candidates:
            return None
        for at in range(0, size - 11, 32768):
            nor.seek(at + 8)
            if nor.read(4) != b'\xef\xbe\xed\xfe':
                continue
            nor.seek(at)
            header = nor.read(0x88)
            if len(header) != 0x88:
                return None
            def word(offset):
                return struct.unpack_from('<I', header, offset)[0]
            def text(offset, expected):
                wanted = expected.encode('ascii') + b'\0'
                return len(wanted) <= 32 and header[offset:offset + len(wanted)] == wanted
            return next((v for v in candidates if
                         v['header_offset'] == at and v['company_id'] == word(0x80) and
                         v['hal_id'] == word(0x84) and text(0x10, v['card_name']) and
                         text(0x30, v['manufacturer']) and
                         (not v['card_ends_at_eof'] or at + word(0x6c) == size)), None)
    return None
