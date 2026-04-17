#include <glib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "auth.h"

/* 
 * Mocking GLib HMAC functions using linker --wrap
 */

GHmac * __wrap_g_hmac_new(GChecksumType checksum_type, const guchar *key, gsize key_len) {
    check_expected(checksum_type);
    check_expected(key);
    check_expected(key_len);
    return (GHmac *)mock();
}

void __wrap_g_hmac_update(GHmac *hmac, const guchar *data, gssize length) {
    check_expected(hmac);
    check_expected(data);
    check_expected(length);
}

const gchar * __wrap_g_hmac_get_string(GHmac *hmac) {
    check_expected(hmac);
    return (const gchar *)mock();
}

void __wrap_g_hmac_unref(GHmac *hmac) {
    check_expected(hmac);
}

/*
 * Test Case: Verify token_auth_login calls HMAC correctly
 */
static void test_auth_login_flow(void **state) {
    (void) state;
    const char *secret = "test-secret-key";
    TokenAuth *auth = token_auth_new(secret, 3600);
    token_auth_add_user(auth, "admin", "password", "admin");

    /* 1. Expect g_hmac_new with secret */
    expect_value(__wrap_g_hmac_new, checksum_type, G_CHECKSUM_SHA256);
    expect_string(__wrap_g_hmac_new, key, secret);
    expect_value(__wrap_g_hmac_new, key_len, strlen(secret));
    will_return(__wrap_g_hmac_new, 0x12345678); /* Fake pointer */

    /* 2. Expect g_hmac_update with payload */
    expect_value(__wrap_g_hmac_update, hmac, 0x12345678);
    expect_any(__wrap_g_hmac_update, data); 
    expect_any(__wrap_g_hmac_update, length);

    /* 3. Expect g_hmac_get_string */
    expect_value(__wrap_g_hmac_get_string, hmac, 0x12345678);
    will_return(__wrap_g_hmac_get_string, "mocked-signature-abc-123");

    /* 4. Expect g_hmac_unref */
    expect_value(__wrap_g_hmac_unref, hmac, 0x12345678);

    char *token = token_auth_login(auth, "admin", "password");
    
    assert_non_null(token);
    /* Verify token format: payload.signature */
    assert_true(strstr(token, ".mocked-signature-abc-123") != NULL);

    g_free(token);
    token_auth_free(auth);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_auth_login_flow),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
