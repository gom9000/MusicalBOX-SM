#!/bin/sh
# MusicalBOX SM: system disk read-only, so pulling the plug cannot damage the SD card.
#
#     sh system/mbx-readonly.sh on       # read-only from the next boot (asks to reboot)
#     sh system/mbx-readonly.sh off      # back to normal (asks to reboot)
#     sh system/mbx-readonly.sh rw       # unlock now, until "ro" or the next boot
#     sh system/mbx-readonly.sh ro       # lock again
#     sh system/mbx-readonly.sh status
#
# While read-only: /tmp, /var/tmp, /var/log and the NetworkManager state live
# in RAM, the log is kept in RAM, swap is off. The web editor unlocks the disk
# by itself for the time of a save. To install a patch or compile: "rw" first.
set -e
MARK="# mbx-readonly"
FSTAB=/etc/fstab
BACKUP=/etc/fstab.before-readonly
JOURNAL=/etc/systemd/journald.conf.d/mbx-readonly.conf

remount() {
    sudo mount -o remount,"$1" /
    if mountpoint -q /boot/firmware; then sudo mount -o remount,"$1" /boot/firmware; fi
}

case "$1" in
on)
    if grep -q "$MARK" "$FSTAB"; then echo "already on (reboot if not active yet)"; exit 0; fi
    sudo cp "$FSTAB" "$BACKUP"
    # "ro" on / and /boot/firmware, tmpfs for what must stay writable.
    sudo awk -v mark="$MARK" '
        ($2 == "/" || $2 == "/boot/firmware") && $4 !~ /(^|,)ro(,|$)/ { $4 = $4 ",ro" }
        { print }
        END {
            print mark " - restore with: sh system/mbx-readonly.sh off"
            print "tmpfs /tmp tmpfs nosuid,nodev,mode=1777,size=64m 0 0"
            print "tmpfs /var/tmp tmpfs nosuid,nodev,mode=1777,size=16m 0 0"
            print "tmpfs /var/log tmpfs nosuid,nodev,mode=0755,size=32m 0 0"
            print "tmpfs /var/lib/NetworkManager tmpfs nosuid,nodev,mode=0700,size=4m 0 0"
        }' "$BACKUP" | sudo tee "$FSTAB.new" > /dev/null
    sudo mv "$FSTAB.new" "$FSTAB"
    sudo mkdir -p "$(dirname "$JOURNAL")"
    printf '[Journal]\nStorage=volatile\nRuntimeMaxUse=16M\n' | sudo tee "$JOURNAL" > /dev/null
    # Things that write on their own: swap file, package and manual-page updates.
    sudo systemctl disable --now dphys-swapfile 2>/dev/null || true
    sudo systemctl disable --now apt-daily.timer apt-daily-upgrade.timer man-db.timer 2>/dev/null || true
    echo "read-only from the next boot: sudo reboot"
    ;;
off)
    if [ ! -f "$BACKUP" ]; then echo "not on (no $BACKUP)"; exit 1; fi
    remount rw
    sudo cp "$BACKUP" "$FSTAB"
    sudo rm -f "$BACKUP" "$JOURNAL"
    sudo systemctl enable dphys-swapfile 2>/dev/null || true
    sudo systemctl enable apt-daily.timer apt-daily-upgrade.timer man-db.timer 2>/dev/null || true
    echo "normal again from the next boot: sudo reboot"
    ;;
rw)
    remount rw
    echo "writable until \"sh system/mbx-readonly.sh ro\" or the next boot"
    ;;
ro)
    sync
    remount ro
    echo "read-only"
    ;;
status)
    findmnt -no TARGET,OPTIONS / /boot/firmware 2>/dev/null || mount | grep -E ' / | /boot/firmware '
    if grep -q "$MARK" "$FSTAB"; then echo "configured: read-only at boot"; else echo "configured: normal (read-write)"; fi
    ;;
*)
    echo "usage: sh system/mbx-readonly.sh on | off | rw | ro | status"
    exit 1
    ;;
esac
