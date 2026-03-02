#include "common.h"
#include "version.h"
#include "hostconfig.h" /* run ./configure */
#include "obj/obj.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

struct option ccopt;
struct cinclpaths cinclpaths[5];

static void
addinclpath(int ord, const char *path)
{
   struct inclpath *p = alloc(&globarena, sizeof *p, 0);
   assert((uint)ord < countof(cinclpaths));
   p->path = path;
   p->next = NULL;
   if (cinclpaths[ord].list) {
      *cinclpaths[ord].tail = p;
   } else {
      cinclpaths[ord].list = p;
   }
   cinclpaths[ord].tail = &p->next;
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

/* "foo.bar" -> "bar"; ".dotfile" -> "" */
static const char *
fileext(const char *path)
{
   const char *dot = NULL;
   assert(path && *path && "empty");

   for (++path; *path; ++path) {
      if (*path == '.') dot = path;
   }
   return dot ? dot+1 : "";
}

enum inft { IFTauto, IFTc, IFTasm, IFTobj, IFTar, IFTdll };

static enum inft
ftdetect(const char *s)
{
   const char *ext = fileext(s);
   if (!strcmp(ext, "c")) return IFTc;
   if (!strcmp(ext, "o")) return IFTobj;
   if (!strcmp(ext, "a")) return IFTar;
   if (!strcmp(ext, "s")) return IFTasm;
   if (!strcmp(ext, "so")) return IFTdll;
   warn(NULL, "assuming %'s is C source file", s);
   return IFTc;
}

static union {
   struct arena a;
   char mem[sizeof(struct arena) + (1<<10)];
} _arenamem;
struct arena *globarena = &_arenamem.a;

/* withext("x/y.c", "o") -> "y.o"; withext("f9", "s") -> "f9.s" */
static const char *
withext(const char *path, const char *ext)
{
   char *res;
   size_t len;
   const char *oext, *file = path;

   while (*path)
      if (*path++ == '/')
         file = path;
   assert(*file && "no filename");
   oext = fileext(file);
   if (!*oext)
      len = strlen(file);
   else
      len = oext - file - 1;
   res = alloc(&globarena, len + 1 + (ext ? strlen(ext) + 1 : 0), 1);
   memcpy(res, file, len);
   if (ext) {
      res[len] = '.';
      memcpy(res + len + 1, ext, strlen(ext));
   } else {
      res[len] = 0;
   }
   return res;
}

struct infile {
   enum inft ft;
   const char *path, *temp;
};
static struct infile infilebuf[16];
static struct task {
   enum outft { OFTexe, OFTdll, OFTobj, OFTasm, OFTc } outft;
   const char *out;
   const char *targ;
   vec_of(struct infile) inf;
   char **runargs;
   vec_of(const char *) linkargs;
   bool verbose, run, syntaxonly;
} task = { .inf = VINIT(infilebuf, countof(infilebuf)) };

static void prihelp(void);

static void
optparse(char **args)
{
   const char *arg, *x;
   enum inft ft = IFTauto;

   while ((arg = *++args)) {
      if (*arg++ != '-' || !*arg) {
         vpush(&task.inf, ((struct infile) {
            ft ? ft : ftdetect(arg-1),
            arg[-1] != '-' ? arg-1 : "/dev/stdin"
         }));
         ft = IFTauto;
         if (task.run) {
            task.runargs = args+1;
            return;
         }
         continue;
      }
      int cinclord;
      if (!strcmp(arg, "help") || !strcmp(arg, "h") || !strcmp(arg, "-help")) {
         prihelp();
         exit(0);
      } else if (!strcmp(arg, "dumpmachine")) {
         pfmt("%s\n", HOST_TRIPLE);
         exit(0);
      } else if (!strcmp(arg, "-version")) {
         pfmt("antcc version "ANTCC_VERSION_STR"\n"
              "target: "HOST_TRIPLE"\n"
              "include paths: "XSTR(HOST_INCLUDE_DIRS)"\n"
              "host cc for linking: " HOST_CC "\n");
         exit(0);
      } else if (!strcmp(arg, "dumpversion")) {
         pfmt("%s\n", ANTCC_VERSION_STR);
         exit(0);
      } else if ((x = optval(arg, "std"))) {
         if (!strcmp(x, "c89") || !strcmp(x, "c90")) ccopt.cstd = STDC89;
         else if (!strcmp(x, "c99")) ccopt.cstd = STDC99;
         else if (!strcmp(x, "c11")) ccopt.cstd = STDC11;
         else if (!strcmp(x, "c2x")) ccopt.cstd = STDC23;
         else if (!strcmp(x, "c23")) ccopt.cstd = STDC23;
         else goto Bad;
      } else if (!strcmp(arg, "pedantic")) {
         ccopt.pedant = 1;
      } else if (!strcmp(arg, "trigraphs")) {
         ccopt.trigraph = 1;
      } else if (*arg == 'd' && arg[1]) {
         /* see common.h§struct option */
         while (*++arg) switch (*arg | 32) {
         case 'p': ccopt.dbg.p = 1; break;
         case 'a': ccopt.dbg.a = 1; break;
         case 'm': ccopt.dbg.m = 1; break;
         case 'o': ccopt.dbg.o = 1; break;
         case 's': ccopt.dbg.s = 1; break;
         case 'i': ccopt.dbg.i = 1; break;
         case 'y': ccopt.dbg.y = 1; break;
         case 'l': ccopt.dbg.l = 1; break;
         case 'r': ccopt.dbg.r = 1; break;
         default: warn(NULL, "-d: invalid debug flag %'c", *arg);
         }
      } else if (*arg == 'o') {
         if (arg[1]) task.out = arg+1;
         else if (args[1]) task.out = *++args;
         else fatal(NULL, "missing path after `-o`");
      } else if (!strcmp(arg, "fsyntax-only")) {
         task.syntaxonly = 1;
      } else if (*arg == 'f') {
         /* -fabc / -fno-abc flags */
         const char *flag = arg+1;
         bool set = 1;
         if (!strncmp(flag, "no-", 3)) {
            set = 0;
            flag += 3;
         }
         if (!strcmp(flag, "pie") || !strcmp(flag, "PIE")) ccopt.pie = set;
         else if (!strcmp(flag, "pic") || !strcmp(flag, "PIC")) ccopt.pic = set;
         else goto Bad;
      } else if (!strcmp(arg, "target") || !strcmp(arg, "-target")) {
         const char *s = *++args;
         if (!s) fatal(NULL, "missing target name");
         task.targ = s;
      } else if (*arg == 'l' || *arg == 'L' || *arg == 'B' || !strcmp(arg, "shared") || !strcmp(arg, "pthread")) {
         vpush(&task.linkargs, arg-1);
      } else if (!strcmp(arg, "v") || !strcmp(arg, "-verbose")) {
         task.verbose = 1;
      } else if (!strcmp(arg, "c")) {
         task.outft = OFTobj;
      } else if (!strcmp(arg, "E")) {
         task.outft = OFTc;
      } else if (!strcmp(arg, "xc")) {
         ft = IFTc;
      } else if (!strcmp(arg, "xo")) {
         ft = IFTobj;
      } else if (!strcmp(arg, "run")) {
         task.run = 1;
         if (task.inf.n > 0) {
            task.runargs = args+1;
            return;
         }
      } else if (*arg == 'g') {
         /* TODO debug info */
      } else if (*arg == 'O') {
         char o = arg[1];
         if (!o || o == 'g') ccopt.o = 0; /* default opts */
         else if (o == '1' || o == 's' || o == 'z') ccopt.o = OPT1;
         else if ((uint)o - '1' < 9) ccopt.o = OPT2;
         else if (o == '0') ccopt.o = OPT0;
         else goto Bad;
      } else if (*arg == 'D' || *arg == 'U') {
         void cpppredef(bool undef, const char *cmd);
         const char *def = arg[1] ? arg+1 : *++args;
         if (!def) fatal(NULL, "macro name missing after `-%c`", *arg);
         cpppredef(*arg == 'U', def);
      } else if (*arg == 'O') {
         /* TODO optimization level */
      } else if (*arg == 'I' || !strcmp(arg, "-include-directory")) {
         const char *p;
         cinclord = CINCL_I;
         if (*arg == 'I' && arg[1]) p = arg+1;
         else CIncl: p = *++args;
         if (!p) fatal(NULL, "missing path after `%s`", arg-1);
         addinclpath(cinclord, p);
      } else if (!strcmp(arg, "iquote")) {
         cinclord = CINCL_iquote;
         goto CIncl;
      } else if (!strcmp(arg, "isystem")) {
         cinclord = CINCL_isystem;
         goto CIncl;
      } else if (!strcmp(arg, "idirafter")) {
         cinclord = CINCL_idirafter;
         goto CIncl;
      } else if (!strcmp(arg, "nostdinc") || !strcmp(arg, "-no-standard-includes")) {
         cinclpaths[CINCLsys].list = NULL;
      } else if (*arg == 'M') {
         ++arg;
         if (*arg == 'F' || *arg == 'T' || *arg == 'Q') {
            const char *p = arg[1] ? arg+1 : *++args;
            if (!p) fatal(NULL, "missing path after `-M%c`", *arg);
         }
         /* TODO depfiles */
      } else if (*arg == 'W') {
         if (!strcmp(arg+1, "error")) {
            ccopt.werror = 1;
         }
         /* TODO warning switches */
      } else if (*arg == 'w') {
         ccopt.wnone = 1;
         /* TODO warning switches */
      } else Bad: warn(NULL, "unrecognized option: %'s", arg-1);
   }

   if (task.inf.n == 0) fatal(NULL, "no input files");

   if (!task.out && !task.syntaxonly) {
      switch (task.outft) {
      case OFTdll:
      case OFTexe: if (!task.run) task.out = "a.out"; break;
      case OFTasm: task.out = withext(task.inf.p[0].path, "s"); break;
      case OFTobj: task.out = withext(task.inf.p[0].path, "o"); break;
      case OFTc: break;
      }
   }
   if (!in_range(task.outft, OFTexe, OFTdll) && task.outft != OFTc && task.inf.n > 1)
      fatal(NULL, "too many input files");
}

static const char *
tempfile(const char *path, const char *ext)
{
   int id;
   static int id2;
   static char sbuf[1024];
   const char *tmpdir;
   const char *file = path;
   struct wbuf fbuf = MEMBUF(sbuf, sizeof sbuf);

   tmpdir = getenv("TMPDIR");
   tmpdir = tmpdir ? tmpdir : "/tmp";
   id = getpid();
   id = id < 0 ? time(NULL) : id;

   while (*path)
      if (*path++ == '/')
         file = path;
   bfmt(&fbuf, "%s/antcc-%x@%u@", tmpdir, id, id2++);
   while (*file++) {
      char c = file[-1];
      if (in_range(c, 'a', 'z') || in_range(c, 'A', 'Z') || in_range(c, '0', '9')
       || c == '_' || c == '-')
      {
         ioputc(&fbuf, c);
      } else if (c == '.') break;
      else ioputc(&fbuf, '_');
   }
   bfmt(&fbuf, "%s%s", &"."[!ext], ext ? ext : "");
   ioputc(&fbuf, 0);
   assert(!fbuf.err);
   return alloccopy(&globarena, fbuf.buf, fbuf.len, 1);
}

static int cc1(const char *out, const char *in);

static const char *tempout;
static pid_t rootp;
static void
mktemps(void) {
   rootp = getpid();
   for (int i = 0; i < task.inf.n; ++i) {
      if (task.inf.p[i].ft == IFTc)
         task.inf.p[i].temp = tempfile(task.inf.p[i].path, "o");
   }
   if (!task.out)
      task.out = tempout = tempfile(task.inf.n > 1 ? "run" : withext(task.inf.p[0].path, NULL), NULL);
}

static void
cleantemps(void)
{
   if (getpid() != rootp) return;
   for (int i = 0; i < task.inf.n; ++i) {
      if (task.inf.p[i].temp) {
         unlink(task.inf.p[i].temp);
         task.inf.p[i].temp = NULL;
      }
   }
   if (tempout)
      unlink(tempout), tempout = NULL;
}
static void
sigcleantemps(int _)
{
   cleantemps();
}

static void
compileobjs(void)
{
   int wstat;
   pid_t p;

   if (!ccopt.dbg.any && !task.syntaxonly) mktemps();
   for (int i = 0; i < task.inf.n; ++i) {
      enum inft ft = task.inf.p[i].ft;
      if (ft == IFTc) {
         if ((p = fork()) < 0) {
            error(NULL, "fork(): %s\n", strerror(errno));
            exit(1);
         } else if (p == 0) {
            exit(cc1(task.inf.p[i].temp, task.inf.p[i].path));
         }
         waitpid(p, &wstat, 0);
         if (!WIFEXITED(wstat)) exit(127);
         if (WEXITSTATUS(wstat) != 0) {
            cleantemps();
            exit(WEXITSTATUS(wstat));
         }
      } else if (ft == IFTobj || ft == IFTar || ft == IFTdll) {
         // passthru
      } else assert(!"not obj");
   }
   if (!ccopt.dbg.any && !task.syntaxonly) {
      atexit(cleantemps);
      signal(SIGINT, sigcleantemps);
      signal(SIGABRT, sigcleantemps);
      signal(SIGILL, sigcleantemps);
   }
}

#include <fcntl.h>

static bool
hasprog(const char *prog)
{
   pid_t p;
   if ((p = fork()) < 0) {
      return 0;
   } else if (p == 0) {
      int nulfd = open("/dev/null", O_WRONLY);
      if (nulfd < 0) return 0;
      for (int fd = 0; fd <= 2; ++fd) {
         dup2(fd, nulfd);
         close(fd);
      }
      const char *cmd[] = {prog, NULL};
      if (execvp(*cmd, (char **)cmd)) {
         close(nulfd);
         error(NULL, "execvp: %s", strerror(errno));
         exit(125);
      }
   }
   int wstat;
   waitpid(p, &wstat, 0);
   if (!WIFEXITED(wstat)) return 0;
   return WEXITSTATUS(wstat) < 125;
}

static bool
iscrosscc(void)
{
   return target.os != HOST_OS || target.arch != HOST_ARCH || target.abi != HOST_ABI;
}

struct cmdargs { vec_of(const char *); };

static void
findlinkcmd(struct cmdargs *cmd)
{
   if (task.targ && iscrosscc()) {
      /* try to find a cross compiling toolchain, e.g. aarch64-linux-gnu-gcc */
      static const char *ccs[] = {"cc", "gcc", "clang"};
      char cross[1024];
      for (int i = 0; i < countof(ccs); ++i) {
         struct wbuf wbuf = MEMBUF(cross, sizeof cross);
         int n = bfmt(&wbuf, "%s-%s", task.targ, ccs[i]);
         assert(n < sizeof cross-1);
         cross[n] = 0;
         if (hasprog(cross)) {
            vpush(cmd, alloccopy(&globarena, cross, n, 1));
            return;
         }
      }
      /* zig cc fallback, which works great as cross compiler toolchain */
      if (hasprog("zig")) {
         vpush(cmd, "zig");
         vpush(cmd, "cc");
         vpush(cmd, "-target");
         vpush(cmd, task.targ);
      } else {
         fatal(NULL, "cannot link to cross-compilation target: no appropiate toolchain installed");
      }
   } else {
      vpush(cmd, HOST_CC);
   }
}

static int
dolink(void)
{
   const char *cmdbuf[100];
   pid_t p;
   int wstat;
   struct cmdargs cmd = VINIT(cmdbuf, countof(cmdbuf));

   findlinkcmd(&cmd);
   if (!strcmp(cmd.p[0], "zig")) {
      note(NULL, "using 'zig cc' as a cross-compiler");
   }
   if (task.outft == OFTdll) {
      vpush(&cmd, "-shared");
   } else if (task.outft == OFTexe) {
      vpush(&cmd, ccopt.pie ? "-pie" : "-no-pie");
   }
   vpush(&cmd, "-o");
   vpush(&cmd, task.out);
   assert(task.inf.n > 0);
   for (int i = 0; i < task.inf.n; ++i) {
      const char *o;
      switch (task.inf.p[i].ft) {
      case IFTc: o = task.inf.p[i].temp; break;
      case IFTobj: case IFTar: case IFTdll:
         o = task.inf.p[i].path; break;
      default: assert(!"link obj?");
      }
      vpush(&cmd, o);
   }
   vpushn(&cmd, task.linkargs.p, task.linkargs.n);
   if (task.verbose) {
      efmt("> ");
      for (int i = 0; i < cmd.n; ++i)
         efmt("%s ", cmd.p[i]);
      efmt("\n");
   }
   vpush(&cmd, NULL);
   if ((p = fork()) < 0) {
      error(NULL, "fork: %s\n", strerror(errno));
      exit(1);
   } else if (p == 0) {
      if (execvp(cmd.p[0], (char **)cmd.p)) {
         error(NULL, "execvp: %s\n", strerror(errno));
         exit(1);
      }
   }
   vfree(&cmd);
   waitpid(p, &wstat, 0);
   if (!WIFEXITED(wstat)) return 127;
   if (WEXITSTATUS(wstat) != 0) {
      error(NULL, "link command failed");
      return 1;
   }
   return 0;
}

static int
dorun(void)
{
   if (target.arch != HOST_ARCH || target.os != HOST_OS) {
      warn(NULL, "'-run' with cross-compiled binary");
   }
   if (task.verbose) {
      efmt("> exec %s", task.out);
      for (char **s = task.runargs; *s; ++s)
         efmt(" %s", *s);
      efmt("\n");
   }
#if _POSIX_C_SOURCE >= 200809L
   /* use fexecve */
   int fexecve(int fd, char *const argv[], char *const envp[]);
   int fd = open(task.out, O_RDONLY);
   if (fd < 0) {
      error(NULL, "open: %s\n", strerror(errno));
      return 1;
   }
   if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
      error(NULL, "fcntl: %s\n", strerror(errno));
      return 1;
   }
   cleantemps();
   extern char **environ;
   fexecve(fd, task.runargs - 1, environ);
   error(NULL, "fexecv: %s\n", strerror(errno));
   return 1;
#else
   pid_t p;
   if ((p = fork()) < 0) {
      error(NULL, "fork(): %s\n", strerror(errno));
      exit(1);
   } else if (p == 0) {
      if (!execv(task.out, task.runargs - 1)) {
         error(NULL, "execv(): %s\n", strerror(errno));
         exit(1);
      }
   }
   int wstat;
   waitpid(p, &wstat, 0);
   if (!WIFEXITED(wstat)) return 127;
   return WEXITSTATUS(wstat);
#endif
}

static int
driver(void)
{
   void cpp(struct wbuf *, const char *);
   if (task.verbose)
      efmt("# Target: %s\n", task.targ ? task.targ : HOST_TRIPLE);
   if (task.syntaxonly)
      task.out = "/dev/null"; // HACK
   if (task.outft == OFTobj) {
      assert(task.inf.n == 1);
      if (task.inf.p[0].ft != IFTc)
         fatal(NULL, "not a C source file: %s", task.inf.p[0].path);
      return cc1(task.out, task.inf.p[0].path);
   } else if (task.outft == OFTc) {
      struct wbuf _buf = {0}, *buf = &bstdout;
      if (task.out) {
         buf = &_buf;
         buf->buf = alloc(&globarena, buf->cap = 1<<12, 1);
         buf->fd = open(task.out, O_CREAT | O_TRUNC | O_WRONLY, 0777);
         if (buf->fd < 0) {
            error(NULL, "open(%'s): %s", task.out, strerror(errno));
            return 1;
         }
      }
      bool ok = 1;
      if (!task.out && task.inf.n == 1)
         cpp(buf, task.inf.p[0].path);
      else for (int i = 0; i < task.inf.n; ++i) {
         pid_t p;
         int wstat;

         if ((p = fork()) < 0) {
            error(NULL, "fork(): %s\n", strerror(errno));
            ok = 0;
         } else if (p == 0) {
            cpp(buf, task.inf.p[i].path);
            exit(0);
         }
         waitpid(p, &wstat, 0);
         if (!WIFEXITED(wstat)) ok = 0;
         ok = ok && WEXITSTATUS(wstat) == 0;
      }
      ioflush(buf);
      if (task.out)
         close(buf->fd);
      return ok ? 0 : 1;
   } else if (task.outft == OFTexe || task.outft == OFTdll) {
      compileobjs();
      if (ccopt.dbg.any || task.syntaxonly) return 0;
      if (!task.run) return dolink();
      int st = dolink();
      if (st == 0) st = dorun();
      return st;
   }
   assert(0);
}

static int
cc1(const char *out, const char *in)
{
   void ccomp(const char *);
   extern int nerror;

   if (task.verbose) efmt("cc1(/*out*/ %'s, /*in*/ %'s)\n", out, in);
   if (!ccopt.dbg.any && !task.syntaxonly) objini(in, out);
   ccomp(in);
   if (!ccopt.dbg.any && !task.syntaxonly && !nerror) objfini();
   return !!nerror;
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

static void
sysinclpaths(void)
{
   static const char *paths[] = {
      HOST_INCLUDE_DIRS
   };
   for (int i = 0; i < countof(paths); ++i)
      addinclpath(CINCLsys, paths[i]);
}

static void
prihelp(void)
{
   pfmt("antcc version "ANTCC_VERSION_STR"\n"
        "Usage: antcc [options] infile(s)...\n"
        "       antcc [options] -run infile [arguments...]\n"
        "       antcc [options] infile(s)... -run [arguments...]\n"
        "Options:\n"
        " -help   \tPrint this help message\n"
        " -std=<..> \tSet C standard (c89, c99, c11, c23)\n"
        " -pedantic \tWarnings for strict standards compliance\n"
        " -d{pamyosilr} \tDebug print IR after {parse, abi, mem, inlining, opts, stack, isel, live, rega}\n"
        " -o <file> \tPlace the output into <file>\n"
        " -v     \tVerbose output\n"
        " -c     \tEmit object file but do not link\n"
        " -run   \tRun compiled sources\n"
        " -Idir  \tAdd include path\n"
        " -Dsym[=val] \tDefine macro\n"
        " -Usym  \tUndefine macro\n"
        " -E     \tPreprocess only\n"
        " -llib  \tLink with library\n"
        " -fpie  \tEmit code for position independent executable\n"
        " -fpic  \tEmit position independent code\n"
        " -O<..> \tSet optimization level (0|g|1|2|s|z)  (default: -Og)\n"
        " -x<c|o> \tSpecify type of next input file (C, object)\n"
        " -W[...] \tTurn on warnings (stub)\n"
        " -Werror \tTurn warnings into errors\n"
        " -w     \tSuppress warnings\n"
        " --version \tPrint version\n"
   );
}

int
main(int argc, char **argv)
{
   globarena->cap = sizeof(_arenamem.mem) - sizeof(struct arena);

   ioinit();
   /* setup defaults */
   detectcolor();
   sysinclpaths();
   ccopt.cstd = STDC11;
   ccopt.pie = 1;
   ccopt.dbgout = &bstdout;

   /* parse cli ags */
   if (argc == 1) {
      prihelp();
      return 1;
   }
   optparse(argv);

   /* global init */
   targ_init(task.targ);
   if (!target.arch)
      fatal(NULL, "unsupported target: %s", task.targ ? task.targ : HOST_TRIPLE);

   return driver();
}

/* vim:set ts=3 sw=3 expandtab: */
