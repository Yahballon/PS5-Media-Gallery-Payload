from pathlib import Path
import re
import hashlib

root = Path(__file__).resolve().parents[1]
version = (root / 'VERSION').read_text().strip()
if not re.fullmatch(r'\d+\.\d+\.\d+', version):
    raise SystemExit('VERSION must use major.minor.patch')
binary = root / 'ps5-media-gallery.elf'
data = binary.read_bytes()
if len(data) < 4096 or data[:4] != b'\x7fELF':
    raise SystemExit('Invalid ELF output')
(root / 'SHA256SUMS').write_text(hashlib.sha256(data).hexdigest() + '  ps5-media-gallery.elf\n')
print('Release v' + version)
