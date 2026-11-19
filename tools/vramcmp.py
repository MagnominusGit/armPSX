# vramcmp.py hardware.png ours.ppm [24]: pixels the test drew on hardware (not its background colour) that
# armPSX drew differently, out of all such pixels. With 24, VRAM holds 24-bit colour: the 16-bit words are
# rebuilt from both pictures and compared as colour bytes.
import subprocess, sys
from collections import Counter

def load(path):
    out = subprocess.run(["convert", path, "-crop", "1024x512+0+0", "+repage", "-depth", "8", "rgb:-"], capture_output=True).stdout
    return [out[i:i + 3] for i in range(0, len(out), 3)]

ref, got = load(sys.argv[1]), load(sys.argv[2])
bg = Counter(ref).most_common(1)[0][0]
drawn = [i for i, p in enumerate(ref) if p != bg]
if len(sys.argv) > 3 and sys.argv[3] == "24":
    word = lambda p: (p[0] >> 3) | (p[1] >> 3) << 5 | (p[2] >> 3) << 10
    bytes_of = lambda px: [b for i in drawn if i < len(px) for b in (word(px[i]) & 0xFF, word(px[i]) >> 8)]
    r, g = bytes_of(ref), bytes_of(got)
    # Bit 15 isn't in the pictures, so every second byte is compared without its top bit.
    d = [abs(a - b) if k % 2 == 0 else min(abs(a - b), 128 - abs(a - b)) for k, (a, b) in enumerate(zip(r, g))]
    bad = sum(1 for x in d if x > 3)
    print(f"{bad} of {len(d)} drawn colour bytes differ by more than 3 (max {max(d, default=0)})")
else:
    bad = [i for i in drawn if i < len(got) and got[i] != ref[i]]
    worst = max((max(abs(a - b) for a, b in zip(got[i], ref[i])) for i in bad), default=0)
    print(f"{len(bad)} of {len(drawn)} drawn px differ" + (f" (max {worst} per channel)" if bad else ""))
