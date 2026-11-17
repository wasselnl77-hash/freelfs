#!/bin/bash
# flboot's firmware entries in real OVMF (text mode over serial): see build/m21/flboot-fw.py. usage: m21-flboot.sh STICK.img
exec python3 -u "$(dirname "$0")/m21/flboot-fw.py" "$@"
