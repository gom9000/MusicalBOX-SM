#!/bin/sh
# MusicalBOX SM: the Pi creates its own Wi-Fi network for the web editor.
#     sh system/mbx-hotspot.sh on  [password]   # network "MusicalBOX-SM", then http://10.42.0.1
#     sh system/mbx-hotspot.sh off               # back to the usual Wi-Fi
#     sh system/mbx-hotspot.sh status
# WARNING: "on" disconnects the Pi from the home Wi-Fi. If you are logged in
# through it (PuTTY over Wi-Fi) the session drops: join "MusicalBOX-SM" with
# the PC and connect again to 10.42.0.1. With a network cable nothing changes.
set -e
NAME=MusicalBOX-SM
case "$1" in
on)
    PASS=${2:-musicalbox}
    if [ ${#PASS} -lt 8 ]; then echo "the password needs at least 8 characters"; exit 1; fi
    if ! nmcli -t -f NAME con show | grep -qx "$NAME"; then
        sudo nmcli con add type wifi ifname wlan0 con-name "$NAME" ssid "$NAME" \
            802-11-wireless.mode ap 802-11-wireless.band bg 802-11-wireless.powersave 2 \
            ipv4.method shared ipv4.addresses 10.42.0.1/24 ipv6.method disabled \
            wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$PASS"
    else
        sudo nmcli con modify "$NAME" wifi-sec.psk "$PASS"
    fi
    # Always on at boot, before any other Wi-Fi.
    sudo nmcli con modify "$NAME" connection.autoconnect yes connection.autoconnect-priority 100
    echo "starting the network $NAME (password: $PASS) - an SSH session over Wi-Fi drops now"
    sudo nmcli con up "$NAME"
    echo "web editor: http://10.42.0.1"
    ;;
off)
    sudo nmcli con modify "$NAME" connection.autoconnect no
    sudo nmcli con down "$NAME" || true
    echo "own network off: the Pi joins the usual Wi-Fi again"
    ;;
status)
    nmcli -f NAME,TYPE,DEVICE,AUTOCONNECT,AUTOCONNECT-PRIORITY con show
    ;;
*)
    echo "usage: sh system/mbx-hotspot.sh on [password] | off | status"
    exit 1
    ;;
esac
