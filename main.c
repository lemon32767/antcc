#include "common.h"
#include "parse.h"
#include "obj.h"
#include <stdlib.h>
#include <unistd.h>

struct option ccopt;

static void
flushstd(void)
{
    ioflush(&bstdout);
    ioflush(&bstderr);
}

/* parse an argument of the form 'opt=abcd'
 * e.g.  arg="foo=bar123"; opt="foo"; returns "bar123" */
static const char *
optval(const char *arg, const char *opt)
{
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
            if (!strcmp(x, "c89") || !strcmp(x, "c90")) ccopt.cstd = STDC89;
            else if (!strcmp(x, "c99")) ccopt.cstd = STDC99;
            else if (!strcmp(x, "c11")) ccopt.cstd = STDC11;
            else if (!strcmp(x, "c2x")) ccopt.cstd = STDC23;
            else if (!strcmp(x, "c23")) ccopt.cstd = STDC23;
            else goto Bad;
        } else if (!strcmp(arg, "pedantic")) {
            ccopt.pedant = 1;
        } else if (*arg == 'd' && arg[1]) {
            /* see common.h§struct option */
            while (*++arg) switch (*arg | 32) {
            case 'p': ccopt.dbg.p = 1; break;
            case 'a': ccopt.dbg.a = 1; break;
            case 'i': ccopt.dbg.i = 1; break;
            case 'r': ccopt.dbg.r = 1; break;
            default: warn(NULL, "-d: invalid debug flag %'c", *arg);
            }
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
    ccopt.cstd = STDC99; /* C99 by default */
    optparse(&file, &targ, argv);
    if (!file) {
        efmt("usage: %s [options] <file>\n", *argv);
        return 1;
    }

    targ_init(targ ? targ : "amd64-sysv");
    objini("a.out");
    initparser(&pr, file);
    parse(&pr);
    if (!nerror) objfini();
}
