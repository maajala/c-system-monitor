#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

/* Minimal test helpers: failures are reported with file/line and counted;
 * TEST_SUMMARY() sets the process exit status. */
#include <math.h>
#include <stdio.h>
#include <string.h>

static int tf_checks = 0;
static int tf_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        tf_checks++;                                                             \
        if (!(cond)) {                                                           \
            tf_failures++;                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))
#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)

#define TEST_SUMMARY()                                                           \
    (printf("%d checks, %d failed\n", tf_checks, tf_failures), tf_failures ? 1 : 0)

#endif
