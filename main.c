#include "common.h"
#include "parse.h"
#include <stdlib.h>
#include <unistd.h>

struct option ccopt;

static void
flushstd(void)
{
    ioflush(&bstdout);
    ioflush(&bstderr);
}

static const char *
optval(const char *arg, const char *opt)
{
    /* arg="foo=bar123"; opt="foo"; returns "bar123" */
    uint n1 = strlen(arg), n2 = strlen(opt);
    if (n1 < n2+1 || memcmp(arg, opt, n2) != 0 || arg[n2] != '=')
        return NULL;
    return arg + n2 + 1;
}

static void
optparse(const char **file, const char **targ, char **args)
{
    const char *arg, *x;
    *file = *targ = NULL;
    while ((arg = *++args)) {
        if (*arg++ != '-') {
            *file = arg-1;
            continue;
        }
        if ((x = optval(arg, "std"))) {
            if (!strcmp(x, "c89")) ccopt.cstd = STDC89;
            else if (!strcmp(x, "c99")) ccopt.cstd = STDC99;
            else if (!strcmp(x, "c11")) ccopt.cstd = STDC11;
            else if (!strcmp(x, "c2x")) ccopt.cstd = STDC23;
            else if (!strcmp(x, "c23")) ccopt.cstd = STDC23;
            else goto Bad;
        } else Bad: warn(NULL, "invalid option: %'s", arg-1);
    }
}

static void
detectcolor(void)
{
    const char *s;
    if (!isatty(STDERR_FILENO)
     || ((s = getenv("NO_COLOR")) && *s)
     || ((s = getenv("TERM")) && !strcmp(s, "dumb")))
        ccopt.nocolor = 1;
}

int
main(int argc, char **argv)
{
    struct parser pr;
    const char *file, *targ;

    atexit(flushstd);
    detectcolor();
    optparse(&file, &targ, argv);
    if (!file) {
        efmt("usage: %s [options] <file>\n", *argv);
        return 1;
    }

    targ_init(targ ? targ : "amd64-sysv");
    initparser(&pr, file);
    parse(&pr);
}
