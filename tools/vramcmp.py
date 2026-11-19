# vramcmp.py hardware.png ours.ppm: pixels the test drew on hardware (not its background colour) that
# armPSX drew differently, out of all such pixels.
import subprocess, sys
from collections import Counter

def load(path):
    out = subprocess.run(["convert", path, "-crop", "1024x512+0+0", "+repage", "-depth", "8", "rgb:-"], capture_output=True).stdout
    return [out[i:i + 3] for i in range(0, len(out), 3)]

ref, got = load(sys.argv[1]), load(sys.argv[2])
bg = Counter(ref).most_common(1)[0][0]
drawn = [i for i, p in enumerate(ref) if p != bg]
bad = [i for i in drawn if i < len(got) and got[i] != ref[i]]
worst = max((max(abs(a - b) for a, b in zip(got[i], ref[i])) for i in bad), default=0)
print(f"{len(bad)} of {len(drawn)} drawn px differ" + (f" (max {worst} per channel)" if bad else ""))
