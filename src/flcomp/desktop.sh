# Start the desktop on tty1 for ordinary users (installed by flcomp).
# SPDX-License-Identifier: MIT
#
# Not exec: if flcomp fails to start you land in the shell (after the crash
# screen, flcrash) instead of a login prompt that comes straight back. Ctrl-Alt-Backspace and
# Log out in the panel make flcomp exit 0, which ends the session too.
# Set FLCOMP_NOSTART=1 (or log in on another tty) to stay in the shell.
# Every program of the desktop (the panel and what it starts, the terminal) inherits this directory:
# start in the home directory, never in what the login chain left (flgreet runs in root's /root,
# which a user cannot read: the terminal's prompt showed it and `ls` said "Permission denied")
[ "$(id -u)" != 0 ] && [ -n "${HOME:-}" ] && [ -d "$HOME" ] && cd "$HOME" 2>/dev/null
# flgreet makes /run/user/UID before login; sessions and the desktop use it
if [ "$(id -u)" != 0 ] && [ -z "${XDG_RUNTIME_DIR:-}" ] && [ -d "/run/user/$(id -u)" ]; then
	XDG_RUNTIME_DIR=/run/user/$(id -u)
	export XDG_RUNTIME_DIR
fi

if [ "$(id -u)" != 0 ] && [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${FLCOMP_NOSTART:-}" ] &&
	[ -z "${SSH_CONNECTION:-}" ] && [ "$(tty 2>/dev/null)" = /dev/tty1 ] &&
	command -v flcomp >/dev/null 2>&1; then
	# flcomp's messages (and those of the panel and every program it starts)
	# go to a log, which the crash screen can show
	flcomp_log=${XDG_RUNTIME_DIR:-/tmp}/flcomp.log
	flcomp_sim=${XDG_RUNTIME_DIR:-/tmp}/flcrash-simulate
	flcomp_crashes= # when it crashed (epoch seconds), kept for a minute
	while :; do
		flcomp_t0=$(date +%s)
		# the log of the run before is kept (flreport collects both)
		[ -s "$flcomp_log" ] && mv -f "$flcomp_log" "$flcomp_log.1" 2>/dev/null
		flcomp 2>"$flcomp_log"
		flcomp_status=$?
		# and a copy outlives the session (the runtime directory is gone after a restart)
		flcomp_state=${XDG_STATE_HOME:-$HOME/.local/state}/freelfs/log
		{ mkdir -p "$flcomp_state" && tail -n 3000 "$flcomp_log" >"$flcomp_state/flcomp.log"; } 2>/dev/null
		[ "$flcomp_status" = 0 ] && exit 0
		flcomp_now=$(date +%s)
		flcomp_secs=$((flcomp_now - flcomp_t0))
		# flcrash-test leaves a note when it wants to pretend something
		if [ -f "$flcomp_sim" ]; then
			read -r flcomp_what <"$flcomp_sim"
			rm -f "$flcomp_sim"
			case $flcomp_what in
			startfail) flcomp_status=1 flcomp_secs=2 ;;
			noseat)
				flcomp_status=1 flcomp_secs=1
				echo "flcomp: cannot open a seat: simulated by flcrash-test" >>"$flcomp_log"
				;;
			nodisplay)
				flcomp_status=1 flcomp_secs=2
				echo "flcomp: no usable display found in /dev/dri (simulated by flcrash-test)" >>"$flcomp_log"
				;;
			loop) flcomp_crashes="$((flcomp_now - 20)) $((flcomp_now - 10))" ;;
			esac
		fi
		# crashes in the last minute, this one included
		flcomp_recent=
		flcomp_n=1
		for flcomp_c in $flcomp_crashes; do
			if [ $((flcomp_now - flcomp_c)) -lt 60 ]; then
				flcomp_recent="$flcomp_recent $flcomp_c"
				flcomp_n=$((flcomp_n + 1))
			fi
		done
		flcomp_crashes="$flcomp_recent $flcomp_now"
		if command -v flcrash >/dev/null 2>&1; then
			flcrash --prog flcomp --status "$flcomp_status" --secs "$flcomp_secs" --crashes "$flcomp_n" \
				--log "$flcomp_log"
			# 0: Enter, start the desktop again; anything else: the shell
			[ "$?" = 0 ] && continue
		else
			tail -n 5 "$flcomp_log" >&2
			echo "flcomp exited with status $flcomp_status" >&2
		fi
		break
	done
	unset flcomp_state flcomp_status flcomp_secs flcomp_t0 flcomp_log flcomp_sim flcomp_crashes flcomp_now \
		flcomp_what flcomp_recent flcomp_n flcomp_c
fi
