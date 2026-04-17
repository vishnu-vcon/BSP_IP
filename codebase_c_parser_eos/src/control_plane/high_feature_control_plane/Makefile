# Makefile — SmartCamera HTTP Server (behind NGINX)
#
# Install dependencies (Debian/Ubuntu):
#   sudo apt-get install \
#       libmicrohttpd-dev \
#       libsqlite3-dev libcurl4-openssl-dev \
#       libqrencode-dev libpng-dev libssl-dev libxcrypt-dev
#
# Build:
#   make
#
# Run (no sudo needed — plain HTTP on port 8080):
#   ./smartcamera_server

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I. -I/tmp/libqrencode-4.1.1
LDFLAGS = -lmicrohttpd -lsqlite3 -lcurl -lcrypt \
          -lqrencode -lpng -lcrypto -lssl -lpthread

SRCS = server.c \
       device_controls.c \
       cJSON.c \
       auth/password_policy.c \
       auth/password_hash.c \
       auth/role_manager.c \
       auth/session_manager.c \
       auth/db_manager.c \
       auth/otp_manager.c \
       auth/activity_logger.c \
       auth/smtp_sender.c

TARGET = smartcamera_server

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
