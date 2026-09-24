#!/data/data/com.termux/files/usr/bin/bash
# Termux:Boot entry — symlink this file to ~/.termux/boot/00-note9-hub
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
sleep 15
bash "$HERE/hub.sh" start
