#!/bin/sh
# MusicalBOX SM: installs and starts the engine and the web editor as services.
# Run from the project folder, as the user who plays (not root):
#     sh system/install-services.sh
# Again after moving the folder. To stop them: sudo systemctl disable --now mbx-engine mbx-control
set -e
DIR=$(cd "$(dirname "$0")/.." && pwd)
USER_NAME=$(id -un)
if [ "$USER_NAME" = root ]; then echo "run it as your user, not with sudo"; exit 1; fi
if [ ! -x "$DIR/build/mbx-engine" ]; then echo "build the engine first: cmake --build build"; exit 1; fi

for unit in mbx-engine mbx-control; do
    sed -e "s|@USER@|$USER_NAME|g" -e "s|@DIR@|$DIR|g" "$DIR/system/$unit.service.in" |
        sudo tee /etc/systemd/system/$unit.service > /dev/null
done
# mbx-control may: unlock the read-only disk for a save, switch the Wi-Fi radio (front panel).
SUDOERS=$(mktemp)
cat > "$SUDOERS" <<EOT
# MusicalBOX SM (system/install-services.sh)
$USER_NAME ALL=(root) NOPASSWD: /usr/bin/mount -o remount\\,rw /, /usr/bin/mount -o remount\\,ro /, /usr/bin/nmcli radio wifi on, /usr/bin/nmcli radio wifi off
EOT
if sudo visudo -cf "$SUDOERS" > /dev/null; then
    sudo install -m 440 "$SUDOERS" /etc/sudoers.d/mbx-control
else
    echo "warning: sudoers rule not valid, not installed"
fi
rm -f "$SUDOERS"
if ! python3 -c "import gpiozero" 2>/dev/null; then
    echo "note: python3-gpiozero is missing - front panel off (sudo apt install python3-gpiozero)"
fi
sudo systemctl daemon-reload
sudo systemctl enable mbx-engine mbx-control
sudo systemctl restart mbx-engine mbx-control
echo "done. Log: journalctl -u mbx-engine -f    status: systemctl status mbx-engine mbx-control"
