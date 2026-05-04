#!/bin/bash

echo "[1/4] Clearing previous gadget configurations to prevent locks..."
# Unbind UDC to release the hardware lock
echo "" > /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null
# Remove the symbolic link
rm /sys/kernel/config/usb_gadget/g1/configs/c.1/ncm.usb0 2>/dev/null
# Remove the function directory
rmdir /sys/kernel/config/usb_gadget/g1/functions/ncm.usb0 2>/dev/null

echo "[2/4] Loading libcomposite and building CDC-NCM gadget..."
modprobe libcomposite
mkdir -p /sys/kernel/config/usb_gadget/g1
cd /sys/kernel/config/usb_gadget/g1

echo 0x1d6b > idVendor  # Linux Foundation
echo 0x0104 > idProduct # Multifunction Composite Gadget
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
echo "123456789" > strings/0x409/serialnumber
echo "NXP" > strings/0x409/manufacturer
echo "i.MX8M Plus CDC-NCM Network Gadget" > strings/0x409/product

mkdir -p configs/c.1/strings/0x409
echo "Config 1: CDC-NCM" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower

echo "[3/4] Linking functions and MAC addresses..."
mkdir -p functions/ncm.usb0
echo "02:11:22:33:44:55" > functions/ncm.usb0/host_addr
echo "02:11:22:33:44:66" > functions/ncm.usb0/dev_addr

ln -s functions/ncm.usb0 configs/c.1/

UDC_NAME=$(ls /sys/class/udc/ | head -n 1)
echo $UDC_NAME > UDC
echo "Gadget bound to $UDC_NAME."

echo "[4/4] Spawning network interface and assigning static IP..."
# Wait a moment for the kernel to register the usb0 interface
sleep 2 

# Flush any weird auto-assigned IPs and force our static IP
ip addr flush dev usb0 2>/dev/null
ip addr add 10.0.0.1/24 dev usb0
ip link set dev usb0 up

echo "✅ Board network ready at IP: 10.0.0.1"
