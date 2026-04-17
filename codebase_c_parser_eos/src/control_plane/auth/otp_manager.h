#ifndef OTP_MANAGER_H
#define OTP_MANAGER_H

#include <stdbool.h>

#define OTP_TTL_SEC    300   /* 5 minutes */
#define OTP_LEN          8   /* 6 digits + null + safety */
#define TOTP_SECRET_MAX 33   /* base32 secret, typically 32 chars + null */
#define URI_MAX        512   /* otpauth URI: ~90 base + 32-char secret + issuer */

/* ---------- Email OTP ---------- */

/* Generate 6-digit OTP string (caller must provide buf[OTP_LEN]). */
void generate_otp(char buf[OTP_LEN]);

/* NOTE: email_otp_send() and email_otp_send_totp_setup() have been moved to
 * auth/smtp_sender.h as part of the SMTP refactor.  They now accept a
 * SmtpServerConfig* and a recipients array instead of EmailOTPConfig*. */

/* ---------- TOTP (Authenticator app) ---------- */

/* Generate a random base32 TOTP secret. out must be TOTP_SECRET_MAX bytes. */
void generate_totp_secret(char out[TOTP_SECRET_MAX]);

/* Generate QR code PNG and return the otpauth URI.
 * qr_out_path must be at least 256 bytes.
 * uri_out must be at least URI_MAX bytes.
 * Uses libqrencode to write the PNG. */
bool generate_qr_code(const char *username,
                      const char *secret,
                      const char *output_dir,
                      char        qr_out_path[256],
                      char        uri_out[URI_MAX]);

/* Verify 6-digit TOTP code. Allows ±1 time window (±30 s).
 * Returns true if valid. */
bool verify_totp(const char *secret, const char *user_code);

#endif /* OTP_MANAGER_H */
