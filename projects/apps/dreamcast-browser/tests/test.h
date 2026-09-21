#ifndef BROWSER_TEST_H
#define BROWSER_TEST_H

#include <stdio.h>
#include <string.h>

extern int test_failures;
extern int test_checks;

#define CHECK(condition) do { \
    test_checks++; \
    if(!(condition)) { \
        test_failures++; \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while(0)

#define CHECK_STR(actual, expected) do { \
    const char *check_actual = (actual); \
    const char *check_expected = (expected); \
    test_checks++; \
    if(!check_actual || strcmp(check_actual, check_expected)) { \
        test_failures++; \
        printf("  FAIL %s:%d: %s\n    got:      \"%s\"\n    expected: \"%s\"\n", \
               __FILE__, __LINE__, #actual, \
               check_actual ? check_actual : "(null)", check_expected); \
    } \
} while(0)

void html_tests(void);
void ui_tests(void);

#endif
