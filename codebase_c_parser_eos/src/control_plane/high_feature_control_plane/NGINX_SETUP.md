# SmartCamera — NXP Board Setup Guide
# Board IP: 192.168.1.160
# Flow: Browser → HTTPS:443 → NGINX (OpenSSL) → HTTP:8080 → C server

===============================================================
YOUR EXACT SETUP
===============================================================

  [Laptop browser]
       |
       | HTTPS port 443  (encrypted — OpenSSL inside NGINX handles this)
       |
  [NXP board: 192.168.1.160]
       |
       +-- NGINX (listens :443, terminates TLS, has the cert+key)
       |       |
       |       | plain HTTP port 8080  (inside the board — safe)
       |       |
       +-- C server (smartcamera_server, listens :8080, no TLS code)
       |
       +-- Cameras


===============================================================
STEP 1 — SSH into the NXP board
===============================================================

  ssh root@192.168.1.160


===============================================================
STEP 2 — Install NGINX on the board
===============================================================

  sudo apt-get update
  sudo apt-get install nginx openssl

  # Verify OpenSSL is linked in NGINX:
  nginx -V 2>&1 | grep openssl


===============================================================
STEP 3 — Generate Certificate for IP 192.168.1.160
===============================================================

  WHY A SPECIAL SCRIPT:
    Normal browsers (Chrome, Firefox) REJECT certs for IP addresses
    unless the cert has a SAN (Subject Alternative Name) with the IP.
    gen_cert.sh adds this. Without it you get ERR_CERT_COMMON_NAME_INVALID
    even after importing the cert.

  chmod +x gen_cert.sh
  sudo bash gen_cert.sh

  # Creates:
  #   /etc/nginx/ssl/smartcamera.crt  <- copy this to your laptop
  #   /etc/nginx/ssl/smartcamera.key  <- stays on board only

  # Verify SAN is present (should show: IP Address:192.168.1.160):
  openssl x509 -in /etc/nginx/ssl/smartcamera.crt -noout -text \
      | grep -A2 "Subject Alternative Name"


===============================================================
STEP 4 — Install NGINX config
===============================================================

  sudo cp nginx_smartcamera.conf /etc/nginx/sites-available/smartcamera
  sudo ln -s /etc/nginx/sites-available/smartcamera \
             /etc/nginx/sites-enabled/smartcamera
  sudo rm -f /etc/nginx/sites-enabled/default

  sudo nginx -t            # must say: syntax is ok
  sudo systemctl enable nginx
  sudo systemctl start nginx


===============================================================
STEP 5 — Build and run the C server
===============================================================

  sudo apt-get install \
      libmicrohttpd-dev libsqlite3-dev libcurl4-openssl-dev \
      libqrencode-dev libpng-dev libssl-dev libxcrypt-dev gcc make

  make

  ./smartcamera_server
  # Expected:
  # [Server] HTTP listening on http://127.0.0.1:8080
  # [Server] NGINX proxies https://0.0.0.0:443 -> here


===============================================================
STEP 6 — Copy cert to laptop and import into browser
===============================================================

  # Run this on your LAPTOP:
  scp root@192.168.1.160:/etc/nginx/ssl/smartcamera.crt ~/smartcamera.crt

  --- Chrome (Linux/Mac) ---
  1. chrome://settings/certificates
  2. Tab "Authorities" -> Import -> select smartcamera.crt
  3. Check "Trust this certificate for identifying websites"
  4. Restart Chrome

  --- Chrome (Windows) ---
  1. chrome://settings/certificates
  2. Tab "Trusted Root Certification Authorities" -> Import
  3. Select smartcamera.crt -> place in Trusted Root CAs
  4. Restart Chrome

  --- Firefox ---
  1. about:preferences#privacy -> View Certificates
  2. Tab "Authorities" -> Import -> smartcamera.crt
  3. Check "Trust this CA to identify websites"
  4. Restart Firefox

  --- Android Chrome ---
  1. Copy smartcamera.crt to phone
  2. Settings -> Security -> Install from storage -> CA Certificate
  3. Select smartcamera.crt


===============================================================
STEP 7 — Open in browser
===============================================================

  https://192.168.1.160

  Before cert import : warning page -> click Advanced -> Proceed
  After cert import  : green padlock, no warning


===============================================================
STEP 8 — Verify
===============================================================

  # From laptop:
  curl -v https://192.168.1.160/        # with cert imported
  curl -k https://192.168.1.160/        # skip cert check (before import)

  # On board:
  sudo systemctl status nginx
  curl http://127.0.0.1:8080/


===============================================================
HOW CERT MATCHING WORKS FOR AN IP
===============================================================

  The cert contains:
    CN  = 192.168.1.160
    SAN IP.1 = 192.168.1.160   <- Chrome checks THIS, not CN

  You type: https://192.168.1.160

  Browser checks:
    1. Does SAN IP.1 match the IP I typed?    YES
    2. Is this cert signed by a CA I trust?
       After import:  YES -> green padlock
       Before import: NO  -> warning page

  Chrome stopped trusting CN for IPs years ago.
  It ONLY looks at SAN for IP matching.
  gen_cert.sh ensures SAN IP.1 = 192.168.1.160 is present.


===============================================================
AUTOSTART ON BOOT
===============================================================

  sudo tee /etc/systemd/system/smartcamera.service << 'EOF'
  [Unit]
  Description=SmartCamera C Server
  After=network.target nginx.service

  [Service]
  ExecStart=/home/user/smartcamera_server
  WorkingDirectory=/home/user/
  Restart=always
  User=user

  [Install]
  WantedBy=multi-user.target
  EOF

  # Change /home/user/ to your actual project path on the board.

  sudo systemctl daemon-reload
  sudo systemctl enable smartcamera
  sudo systemctl start smartcamera


===============================================================
WHAT CHANGED IN server.c
===============================================================

  BEFORE                           AFTER
  Port 443 HTTPS                   Port 8080 plain HTTP
  Loaded server.crt into RAM       No cert loading
  Loaded server.key into RAM       No key loading
  MHD_USE_TLS flag                 No MHD_USE_TLS
  GnuTLS did crypto                NGINX+OpenSSL do crypto
  Needed sudo to run               No sudo needed
  -lgnutls in Makefile             Removed from Makefile
