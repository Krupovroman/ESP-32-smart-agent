#include <stdio.h>
#include <windows.h>
#include "mocks.h"

void test_misc(void);
void test_models(void);
void test_ai(void);
void test_tts(void);

int main(void)
{
    SetConsoleOutputCP(CP_UTF8);
    test_misc();
    test_models();
    test_ai();
    test_tts();
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
