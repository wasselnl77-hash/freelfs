#!/bin/sh
# flpower saver on/off against a made-up /sys/devices/system/cpu (FLPOWER_CPU, FLPOWER_STATE:
# honoured only when not running as root). `make check`.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
export FLPOWER_CPU=$dir/cpu FLPOWER_STATE=$dir/state
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
mk() {	# cpu governor epp
	d=$FLPOWER_CPU/cpu$1/cpufreq
	mkdir -p "$d"
	echo "$2" >"$d/scaling_governor"
	echo "performance powersave schedutil" >"$d/scaling_available_governors"
	if [ -n "$3" ]; then
		echo "$3" >"$d/energy_performance_preference"
		echo "default performance balance_performance balance_power power" >"$d/energy_performance_available_preferences"
	fi
}
mk 0 schedutil balance_performance
mk 1 performance balance_performance
mk 2 schedutil ""
mkdir -p "$FLPOWER_CPU/cpufreq" "$FLPOWER_CPU/cpuidle"	# not CPUs
g() { cat "$FLPOWER_CPU/cpu$1/cpufreq/scaling_governor"; }
e() { cat "$FLPOWER_CPU/cpu$1/cpufreq/energy_performance_preference" 2>/dev/null; }

./flpower saver on && ok "saver on runs" || bad "saver on failed"
[ "$(g 0)" = powersave ] && [ "$(g 1)" = powersave ] && [ "$(g 2)" = powersave ] && ok "governors are powersave" || bad "governors: $(g 0) $(g 1) $(g 2)"
[ "$(e 0)" = power ] && [ "$(e 1)" = power ] && ok "energy preference is power" || bad "epp: $(e 0) $(e 1)"
[ -z "$(e 2)" ] && ok "a CPU without the preference file is left alone" || bad "epp on cpu2"
./flpower saver on
./flpower saver off && ok "saver off runs" || bad "saver off failed"
[ "$(g 0)" = schedutil ] && [ "$(g 1)" = performance ] && [ "$(g 2)" = schedutil ] && ok "governors are back (a second 'on' did not overwrite the saved ones)" || bad "restore: $(g 0) $(g 1) $(g 2)"
[ "$(e 0)" = balance_performance ] && [ "$(e 1)" = balance_performance ] && ok "energy preference is back" || bad "epp restore: $(e 0) $(e 1)"
[ ! -e "$FLPOWER_STATE" ] && ok "state file removed" || bad "state file left"
./flpower saver off && ok "off without on does nothing" || bad "off without on"
./flpower saver sideways 2>/dev/null; [ $? = 2 ] && ok "other words are refused" || bad "bad word accepted"
./flpower 2>/dev/null; [ $? = 2 ] && ok "no words: usage" || bad "usage"
# a governor the kernel does not offer is not written
echo "performance schedutil" >"$FLPOWER_CPU/cpu0/cpufreq/scaling_available_governors"
./flpower saver on
[ "$(g 0)" = schedutil ] && ok "no powersave governor: unchanged" || bad "wrote an unavailable governor"
./flpower saver off
[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
