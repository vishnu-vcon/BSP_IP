#!/bin/bash

IFACE="enx021122334455"
IP="10.0.0.2/24"
CON_NAME="imx8-usb-link"

echo "[1/3] Disconnecting active interface and clearing old NetworkManager profiles..."
sudo nmcli device disconnect $IFACE 2>/dev/null
sudo nmcli connection delete "$CON_NAME" 2>/dev/null

echo "[2/3] Creating permanent static profile for $IFACE..."
sudo nmcli connection add type ethernet ifname $IFACE con-name "$CON_NAME" ipv4.method manual ipv4.addresses $IP

echo "[3/3] Bringing the connection online..."
sudo nmcli connection up "$CON_NAME"

echo "✅ PC network ready at IP: 10.0.0.2"
