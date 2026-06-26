#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static void test_##name(void)
#define RUN(name) do { \
    fprintf(stderr, "  " #name "... "); \
    test_##name(); \
    fprintf(stderr, "OK\n"); \
} while (0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "\nFAIL: %s:%d: expected %lld, got %lld\n", \
                __FILE__, __LINE__, (long long)(b), (long long)(a)); \
        exit(1); \
    } \
} while (0)

#define ASSERT_NE(a, b) do { \
    if ((a) == (b)) { \
        fprintf(stderr, "\nFAIL: %s:%d: expected != %lld\n", \
                __FILE__, __LINE__, (long long)(b)); \
        exit(1); \
    } \
} while (0)

#define ASSERT_TRUE(c) do { \
    if (!(c)) { \
        fprintf(stderr, "\nFAIL: %s:%d: expected true\n", \
                __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_FALSE(c) do { \
    if ((c)) { \
        fprintf(stderr, "\nFAIL: %s:%d: expected false\n", \
                __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_STREQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        fprintf(stderr, "\nFAIL: %s:%d: expected \"%s\", got \"%s\"\n", \
                __FILE__, __LINE__, (b), (a)); \
        exit(1); \
    } \
} while (0)

/* Compile-time assertion macro */
#define ASSERT_SIZEOF(type, expected) \
    _Static_assert(sizeof(type) == (expected), \
                   "sizeof(" #type ") must be " #expected)

#endif /* TEST_RUNNER_H */
