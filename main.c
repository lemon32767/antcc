#include "parse.h"
#include <stdlib.h>

struct option ccopt;

static void
flushstd(void)
{
    ioflush(&bstdout);
    ioflush(&bstderr);
}

int
main(int argc, char **argv)
{
    struct parser pr;

    atexit(flushstd);
    if (argc < 2) {
        efmt("usage: %s [options] <file>\n", *argv);
        return 1;
    }

    targ_init("amd64-sysv");
    initparser(&pr, argv[1]);
    parse(&pr);
}
