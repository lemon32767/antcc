/* EXPECT:
x = 666
*/
#include <setjmp.h>
#include <stdio.h>

int main(int argc, char** argv) {
    volatile int x = 42;
    jmp_buf jb;
    if (setjmp(jb)) {
        printf("x = %d\n", x);
        return 0;
    }
    x = 666;
    longjmp(jb, 1);
    printf("Should not get here.\n");
    return 1;
}
