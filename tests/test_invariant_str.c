#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>

extern char* strcat(char* dest, const char* source);

static jmp_buf jump_buffer;
static int segfault_caught = 0;

void segfault_handler(int sig) {
    segfault_caught = 1;
    longjmp(jump_buffer, 1);
}

START_TEST(test_strcat_buffer_overflow_protection)
{
    // Invariant: Buffer reads never exceed declared length; strcat must not overflow
    const char *payloads[] = {
        "A",                                    // valid: single char
        "normal_string",                        // valid: typical input
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",   // boundary: 34 chars (near 32-byte buffer)
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"  // exploit: 64 chars (2x overflow)
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    signal(SIGSEGV, segfault_handler);

    for (int i = 0; i < num_payloads; i++) {
        char dest[32];
        memset(dest, 0, sizeof(dest));
        strcpy(dest, "prefix_");

        segfault_caught = 0;
        if (setjmp(jump_buffer) == 0) {
            strcat(dest, payloads[i]);
            // If we reach here without segfault, verify dest is null-terminated
            ck_assert(dest[sizeof(dest) - 1] == '\0' || strlen(dest) < sizeof(dest));
        } else {
            // Segfault caught: strcat violated buffer bounds
            ck_abort_msg("strcat caused buffer overflow on payload %d", i);
        }
    }

    signal(SIGSEGV, SIG_DFL);
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_strcat_buffer_overflow_protection);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}