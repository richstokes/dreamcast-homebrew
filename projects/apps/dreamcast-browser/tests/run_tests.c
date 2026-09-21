#include "test.h"

int test_failures;
int test_checks;

static void run(const char *name, void (*suite)(void)) {
    int before = test_failures;
    printf("%s\n", name);
    suite();
    if(test_failures == before) printf("  ok\n");
}

int main(void) {
    run("html", html_tests);
    run("osk, address bar, bookmarks", ui_tests);
    printf("%d checks, %d failed\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
