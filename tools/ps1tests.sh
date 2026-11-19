#!/bin/sh
# ps1tests.sh psxrun bios tests-bin ps1-tests [frames]: runs JaCzekanski's ps1-tests and compares each
# with its real-hardware log (psx.log) or VRAM picture (vram.png).
RUN=$1 BIOS=$2 BIN=$3 REF=$4 FRAMES=${5:-600}
OUT=${OUT:-/tmp/ps1tests}
mkdir -p "$OUT"
TOOLS=$(cd "$(dirname "$0")" && pwd)
cd "$BIN" || exit 1
find . -name "*.exe" | sort | while read exe; do
	dir=$(dirname "$exe" | sed 's|^\./||')
	name=$(basename "$exe" .exe)
	case "$dir" in tools/*|cdrom/*|input/*) continue;; esac
	log="$OUT/$(echo "$dir/$name" | tr / _)"
	PSX_TTY=1 PSXRUN_VRAM="$log.ppm" timeout 120 "$RUN" "$BIOS" "$FRAMES" "$exe" 2>&1 | sed -n 's/^tty: //p' > "$log.txt"
	result="no reference"
	if [ -f "$REF/$dir/psx.log" ]; then
		sed 's/^% \{0,1\}//; s/[ \r]*$//' "$REF/$dir/psx.log" | grep . > "$log.ref"
		sed 's/[ \r]*$//' "$log.txt" > "$log.got"
		total=$(grep -c . "$log.ref")
		missing=$(grep -vFxf "$log.got" "$log.ref" | grep -c .)
		result="log: $missing of $total hardware lines missing"
	fi
	png=$(ls "$REF/$dir"/vram*.png 2>/dev/null | head -1)
	if [ -n "$png" ]; then
		result="$result, vram $(python3 -I "$TOOLS/vramcmp.py" "$png" "$log.ppm")"
	fi
	echo "$dir/$name: $result"
done
