#include <gst/gst.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "encoder_builder.h"

/* 
 * Mocking GStreamer element creation
 */
GstElement * __wrap_gst_element_factory_make(const gchar *factoryname, const gchar *name) {
    check_expected(factoryname);
    check_expected(name);
    /* In a real test we'd return a fake object, 
       but for this verification we just return a tag */
    return (GstElement *)mock();
}

/* Mocking property setting to avoid crashes on fake pointers */
void g_object_set(gpointer object, const gchar *first_property_name, ...) {
    /* We could use check_expected here too if we wanted to verify parameters */
    (void)object;
    (void)first_property_name;
}

/*
 * Test Case: Verify standard H.264 element chain creation
 */
static void test_h264_builder_flow(void **state) {
    (void) state;
    int out_count = 0;
    const char *uid = "test_cam";
    
    /* 1. Expect Queue */
    expect_string(__wrap_gst_element_factory_make, factoryname, "queue");
    expect_string(__wrap_gst_element_factory_make, name, "q_test_cam");
    will_return(__wrap_gst_element_factory_make, 0x11);

    /* 2. Expect Scaler (fallback to videoconvert in mock env) */
    expect_string(__wrap_gst_element_factory_make, factoryname, "videoconvert");
    expect_string(__wrap_gst_element_factory_make, name, "branch_scale_test_cam");
    will_return(__wrap_gst_element_factory_make, 0x22);

    /* 3. Expect Res Filter */
    expect_string(__wrap_gst_element_factory_make, factoryname, "capsfilter");
    expect_string(__wrap_gst_element_factory_make, name, "res_test_cam");
    will_return(__wrap_gst_element_factory_make, 0x33);

    /* 4. Expect Rate Controller */
    expect_string(__wrap_gst_element_factory_make, factoryname, "videorate");
    expect_string(__wrap_gst_element_factory_make, name, "rate_test_cam");
    will_return(__wrap_gst_element_factory_make, 0x44);

    /* 5. Expect FPS Filter */
    expect_string(__wrap_gst_element_factory_make, factoryname, "capsfilter");
    expect_string(__wrap_gst_element_factory_make, name, "fps_caps_test_cam");
    will_return(__wrap_gst_element_factory_make, 0x55);

    /* 6. Expect Encoder (H264 hardware fallback to x264enc) */
    expect_string(__wrap_gst_element_factory_make, factoryname, "v4l2h264enc");
    expect_string(__wrap_gst_element_factory_make, name, "enc_test_cam");
    will_return(__wrap_gst_element_factory_make, NULL);
    expect_string(__wrap_gst_element_factory_make, factoryname, "x264enc");
    expect_string(__wrap_gst_element_factory_make, name, "enc_test_cam");
    will_return(__wrap_gst_element_factory_make, 0x66);

    /* 7. Expect Parser */
    expect_string(__wrap_gst_element_factory_make, factoryname, "h264parse");
    expect_string(__wrap_gst_element_factory_make, name, "parse_test_cam");
    will_return(__wrap_gst_element_factory_make, 0x77);

    GstElement **elems = build_encoder_elements(uid, "h264", 30, 1920, 1080, 4000000, "cbr", FALSE, FALSE, &out_count);
    
    assert_int_equal(out_count, 7);
    assert_ptr_equal(elems[0], 0x11);
    assert_ptr_equal(elems[6], 0x77);

    g_free(elems);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_h264_builder_flow),
    };
    /* We don't call gst_init() because we mocked the core functions */
    return cmocka_run_group_tests(tests, NULL, NULL);
}
