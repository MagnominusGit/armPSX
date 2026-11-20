#!/bin/sh
# profsym.sh psxrun < profile: psxrun's PSXRUN_PROF host samples summed by function
BIN=$1
awk '/profReport at/ {print "base", $NF} /^prof host/ {print $3, $4}' | {
	read _ self
	ref=$(${NM:-nm} "$BIN" | awk '$3 == "profReport" {print $1}')
	while read a n; do
		printf '%x %s\n' $((0x$a - 0x$self + 0x$ref)) "$n"
	done
} | while read a n; do
	echo "$(${ADDR2LINE:-addr2line} -f -e "$BIN" 0x$a | head -1) $n"
done | awk '{s[$1] += $2; t += $2} END {for (f in s) printf "%6.1f%% %s\n", 100 * s[f] / t, f}' | sort -rn | head -25
