/*
 * auth/otp_manager.c
 *
 * Two OTP mechanisms:
 *   1. EmailOTP  — 6-digit random code sent via SMTP (libcurl)
 *                  Actual email sending is handled by auth/smtp_sender.c.
 *   2. TOTP      — Authenticator app (RFC 6238 TOTP)
 *                  QR code PNG generated with libqrencode.
 *
 * Dependencies:
 *   - libqrencode   (-lqrencode)   for QR PNG generation
 *   - libpng        (-lpng)        for writing PNG files
 *   - openssl       (-lcrypto)     for HMAC-SHA1 used in TOTP
 */

#include "otp_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* OpenSSL HMAC for TOTP */
#include <openssl/hmac.h>
#include <openssl/evp.h>

/* libqrencode for QR PNG */
#include <qrencode.h>
#include <png.h>

#define APP_NAME "SmartCamera"

/* ═══════════════════════════════════════════════════════════
 *  Utilities
 * ═══════════════════════════════════════════════════════════ */

void generate_otp(char buf[OTP_LEN])
{
    srand((unsigned)time(NULL) ^ (unsigned)clock());
    int code = 100000 + rand() % 900000;
    snprintf(buf, OTP_LEN, "%06d", code % 1000000);  /* "%06d" guarantees exactly 6 digits+null */
}

/* ═══════════════════════════════════════════════════════════
 *  base32 helpers (for TOTP secret decode / encode)
 * ═══════════════════════════════════════════════════════════ */

static const char B32_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

void generate_totp_secret(char out[TOTP_SECRET_MAX])
{
    /* Generate 20 random bytes → 32 base32 chars */
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) { out[0] = '\0'; return; }
    uint8_t raw[20];
    fread(raw, 1, sizeof(raw), f);
    fclose(f);

    int i = 0, j = 0;
    uint32_t buf = 0;
    int bits = 0;
    while (i < 20 && j < TOTP_SECRET_MAX - 1) {
        buf = (buf << 8) | raw[i++];
        bits += 8;
        while (bits >= 5 && j < TOTP_SECRET_MAX - 1) {
            bits -= 5;
            out[j++] = B32_CHARS[(buf >> bits) & 0x1F];
        }
    }
    out[j] = '\0';
    printf("  [TOTP] secret generated: %s\n", out);
}

/* Decode base32 string → bytes. Returns number of bytes written. */
static int base32_decode(const char *encoded, uint8_t *decoded, int max_bytes)
{
    int buf = 0, bits = 0, count = 0;
    for (int i = 0; encoded[i] && count < max_bytes; i++) {
        char c = encoded[i];
        int val = -1;
        if (c >= 'A' && c <= 'Z') val = c - 'A';
        else if (c >= '2' && c <= '7') val = c - '2' + 26;
        else if (c >= 'a' && c <= 'z') val = c - 'a'; /* lowercase tolerated */
        if (val < 0) continue;
        buf  = (buf << 5) | val;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            decoded[count++] = (buf >> bits) & 0xFF;
        }
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════
 *  TOTP  (RFC 6238 / RFC 4226 HOTP with T = floor(time/30))
 * ═══════════════════════════════════════════════════════════ */

static uint32_t totp_at(const uint8_t *key, int key_len, int64_t T)
{
    /* T as big-endian 8-byte counter */
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = T & 0xFF;
        T >>= 8;
    }

    uint8_t  hmac_out[20];
    unsigned hmac_len = 20;
    HMAC(EVP_sha1(), key, key_len, msg, 8, hmac_out, &hmac_len);

    int offset = hmac_out[19] & 0x0F;
    uint32_t code = ((hmac_out[offset]     & 0x7F) << 24)
                  | ((hmac_out[offset + 1] & 0xFF) << 16)
                  | ((hmac_out[offset + 2] & 0xFF) <<  8)
                  | ((hmac_out[offset + 3] & 0xFF));
    return code % 1000000;
}

bool verify_totp(const char *secret, const char *user_code)
{
    uint8_t key[32];
    int key_len = base32_decode(secret, key, sizeof(key));
    if (key_len == 0) return false;

    int64_t T = (int64_t)time(NULL) / 30;
    int entered = atoi(user_code);

    for (int window = -1; window <= 1; window++) {
        if ((int)totp_at(key, key_len, T + window) == entered) {
            printf("  [TOTP] verify  code=%s  result=true\n", user_code);
            return true;
        }
    }
    printf("  [TOTP] verify  code=%s  result=false\n", user_code);
    return false;
}

/* ═══════════════════════════════════════════════════════════
 *  QR code PNG (libqrencode + libpng)
 * ═══════════════════════════════════════════════════════════ */

bool generate_qr_code(const char *username,
                      const char *secret,
                      const char *output_dir,
                      char        qr_out_path[256],
                      char        uri_out[URI_MAX])
{
    /* Build otpauth URI */
    snprintf(uri_out, URI_MAX,
             "otpauth://totp/%s:%s?secret=%s&issuer=%s",
             APP_NAME, username, secret, APP_NAME);

    /* Output path */
    snprintf(qr_out_path, 256, "%s/qr_%s.png", output_dir, username);

    /* Encode */
    QRcode *qr = QRcode_encodeString(uri_out, 0, QR_ECLEVEL_M,
                                     QR_MODE_8, 1);
    if (!qr) {
        fprintf(stderr, "  [TOTP] QRcode_encodeString failed\n");
        return false;
    }

    /* Write PNG via libpng */
    const int scale = 8;  /* each module = 8×8 pixels */
    int size = qr->width * scale;

    FILE *fp = fopen(qr_out_path, "wb");
    if (!fp) {
        fprintf(stderr, "  [TOTP] cannot open %s for writing\n", qr_out_path);
        QRcode_free(qr);
        return false;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              NULL, NULL, NULL);
    png_infop   info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {
        fclose(fp);
        QRcode_free(qr);
        return false;
    }
    png_init_io(png, fp);
    png_set_IHDR(png, info, size, size, 8,
                 PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    uint8_t *row = malloc(size);
    for (int y = 0; y < qr->width; y++) {
        for (int sy = 0; sy < scale; sy++) {
            for (int x = 0; x < qr->width; x++) {
                uint8_t pix = (qr->data[y * qr->width + x] & 1) ? 0 : 255;
                for (int sx = 0; sx < scale; sx++)
                    row[x * scale + sx] = pix;
            }
            png_write_row(png, row);
        }
    }
    free(row);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    QRcode_free(qr);

    printf("  [TOTP] QR saved → %s\n", qr_out_path);
    return true;
}
