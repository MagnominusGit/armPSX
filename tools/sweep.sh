#!/bin/sh
# sweep.sh list [frames] [shots] [press]: each game in armPSX and Beetle; pictures in sweep/<n>-{a,b}-<frame>.ppm
cd /storage/psxtest || exit 1
FRAMES=${2:-3600} AT=${3:-1200,2400,3600} PRESS=${4:-}
mkdir -p sweep
n=0
while IFS= read -r g; do
	n=$((n + 1))
	p="/storage/roms/psx/$g"
	b="$p"
	[ -d "$p" ] && b="$p/$(basename "$p")"
	echo "$n|$g" >> sweep/index.txt
	bios=scph5501.bin; case "$g" in *"(Europe"*) bios=scph5502.bin;; *"(Japan"*) bios=scph5500.bin;; esac
	( PSXRUN_PRESS=$PRESS PSXRUN_SHOTS=sweep/$n-a PSXRUN_AT=$AT timeout 400 ./psxrun /storage/roms/bios/$bios $FRAMES "$p" > sweep/$n-a.log 2>&1 ) &
	( LR_PRESS=$PRESS SYSDIR=/storage/roms/bios LR_SHOTS=sweep/$n-b LR_AT=$AT timeout 400 ./lrshot /usr/lib/libretro/beetle_psx_libretro.so "$b" $FRAMES > sweep/$n-b.log 2>&1 ) &
	if [ $((n % 4)) = 0 ]; then wait; fi
done < "$1"
wait
echo done > sweep/done
