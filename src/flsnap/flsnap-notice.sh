# /etc/profile.d/flsnap-notice.sh -- tell people on a text console when the
# system runs from an earlier state (a snapshot).
case $- in
*i*)
	if grep -q '^[0-9]* [0-9]* [0-9:]* /@snapshots/[0-9]* / ' /proc/self/mountinfo 2> /dev/null; then
		echo "NOTE: this is an earlier state of the system (a snapshot, read-only)."
		echo "      Make it the current state:  flsnap rollback      Back to normal: restart."
	fi
	;;
esac
