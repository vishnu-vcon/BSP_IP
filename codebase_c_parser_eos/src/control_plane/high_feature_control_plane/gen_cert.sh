#!/bin/bash
BOARD_IP="192.168.1.160"
CERT_DIR="/etc/nginx/ssl"
mkdir -p "$CERT_DIR"

# Step 1: Create CA key and cert (CA:TRUE)
openssl genrsa -out "$CERT_DIR/ca.key" 2048
openssl req -x509 -new -nodes -key "$CERT_DIR/ca.key" -sha256 -days 825 \
    -out "$CERT_DIR/ca.crt" \
    -subj "/CN=SmartCamera-CA/O=SmartCamera/C=IN"

# Step 2: Create server key and CSR
openssl genrsa -out "$CERT_DIR/smartcamera.key" 2048
openssl req -new -key "$CERT_DIR/smartcamera.key" \
    -out "$CERT_DIR/smartcamera.csr" \
    -subj "/CN=$BOARD_IP/O=SmartCamera/C=IN"

# Step 3: Sign server cert with CA
cat > /tmp/san.ext << EXTEOF
subjectAltName=IP:$BOARD_IP
basicConstraints=CA:FALSE
EXTEOF

openssl x509 -req -in "$CERT_DIR/smartcamera.csr" \
    -CA "$CERT_DIR/ca.crt" -CAkey "$CERT_DIR/ca.key" \
    -CAcreateserial -out "$CERT_DIR/smartcamera.crt" \
    -days 825 -sha256 -extfile /tmp/san.ext

chmod 600 "$CERT_DIR/smartcamera.key" "$CERT_DIR/ca.key"
chmod 644 "$CERT_DIR/smartcamera.crt" "$CERT_DIR/ca.crt"
echo "Done! Import ca.crt into Firefox (not smartcamera.crt)"
