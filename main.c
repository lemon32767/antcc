#include "common.h"
#include "hostconfig.h"
#include "obj/obj.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

struct option ccopt;
struct inclpaths *cinclpaths;

static void
addinclpath(const char *path)
{
   struct inclpaths *p = alloc(&globarena, sizeof *cinclpaths, 0);
   p->next = cinclpaths;
   p->path = path;
   cinclpaths = p;
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

enum inft { IFTauto, IFTc, IFTasm, IFTobj, IFTar };

static enum inft
ftdetect(const char *s)
{
   const char *ext = fileext(s);
   if (!strcmp(ext, "c")) return IFTc;
   if (!strcmp(ext, "o")) return IFTobj;
   if (!strcmp(ext, "a")) return IFTar;
   if (!strcmp(ext, "s")) return IFTasm;
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

static struct task {
   enum outft { OFTexe, OFTdll, OFTobj, OFTasm, OFTc } outft;
   const char *out;
   const char *targ;
   const char *inf[64];
   enum inft inft[64];
   char **runargs;
   vec_of(const char *) linkargs;
   int ninf;
   bool verbose, run;
} task;

static void prihelp(void);

static void
optparse(char **args)
{
   const char *arg, *x;
   enum inft ft = IFTauto;

   while ((arg = *++args)) {
      if (*arg++ != '-' || !*arg) {
         assert(task.ninf < countof(task.inf) && "too many infiles");
         task.inf[task.ninf] = arg[-1] != '-' ? arg-1 : "/dev/stdin";
         task.inft[task.ninf] = ft ? ft : ftdetect(arg-1);
         ++task.ninf;
         ft = IFTauto;
         if (task.run) {
            task.runargs = args+1;
            return;
         }
         continue;
      }
      if (!strcmp(arg, "help") || !strcmp(arg, "h") || !strcmp(arg, "-help")) {
         prihelp();
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
         case 'i': ccopt.dbg.i = 1; break;
         case 'l': ccopt.dbg.l = 1; break;
         case 'r': ccopt.dbg.r = 1; break;
         default: warn(NULL, "-d: invalid debug flag %'c", *arg);
         }
      } else if (*arg == 'o') {
         if (arg[1]) task.out = arg+1;
         else if (args[1]) task.out = *++args;
         else fatal(NULL, "missing path after `-o`");
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
      } else if (*arg == 'l') {
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
         if (task.ninf > 0) {
            task.runargs = args+1;
            return;
         }
      } else if (*arg == 'g') {
         /* TODO debug info */
      } else if (*arg == 'O') {
         if (!arg[1] || (uint)arg[1] - '1' < 9) ccopt.o = OPT1;
         else if (arg[1] == '0') ccopt.o = OPT0;
         else goto Bad;
      } else if (*arg == 'D' || *arg == 'U') {
         void cpppredef(bool undef, const char *cmd);
         const char *def = arg[1] ? arg+1 : *++args;
         if (!def) fatal(NULL, "macro name missing after `-%c`", *arg);
         cpppredef(*arg == 'U', def);
      } else if (*arg == 'O') {
         /* TODO optimization level */
      } else if (*arg == 'I') {
         const char *p = arg[1] ? arg+1 : *++args;
         if (!p) fatal(NULL, "missing path after `-I`");
         else addinclpath(p);
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
      } else Bad: warn(NULL, "unrecognized option: %'s", arg-1);
   }

   if (!task.ninf) fatal(NULL, "no input files");

   if (!task.out) {
      switch (task.outft) {
      case OFTdll:
      case OFTexe: if (!task.run) task.out = "a.out"; break;
      case OFTasm: task.out = withext(*task.inf, "s"); break;
      case OFTobj: task.out = withext(*task.inf, "o"); break;
      case OFTc: break;
      }
   }
   if (!in_range(task.outft, OFTexe, OFTdll) && task.outft != OFTc && task.ninf > 1)
      fatal(NULL, "too many input files");
}

static const char *
tempfile(const char *path, const char *ext)
{
   long time(void *);
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

static const char *tempobj[countof(task.inf)], *tempout;
static void
mktemps(void) {
   for (int i = 0; i < task.ninf; ++i) {
      if (task.inft[i] == IFTc)
         tempobj[i] = tempfile(task.inf[i], "o");
   }
   if (!task.out)
      task.out = tempout = tempfile(task.ninf > 1 ? "run" : withext(task.inf[0], NULL), NULL);
}
static void
cleantemps(void)
{
   for (int i = 0; i < task.ninf; ++i) {
      if (tempobj[i]) unlink(tempobj[i]), tempobj[i] = NULL;
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

   if (!ccopt.dbg.any) mktemps();
   for (int i = 0; i < task.ninf; ++i) {
      if (task.inft[i] == IFTc) {
         if ((p = fork()) < 0) {
            error(NULL, "fork(): %s\n", strerror(errno));
            exit(1);
         } else if (p == 0) {
            exit(cc1(tempobj[i], task.inf[i]));
         }
         waitpid(p, &wstat, 0);
         if (!WIFEXITED(wstat)) exit(127);
         if (WEXITSTATUS(wstat) != 0) {
            cleantemps();
            exit(WEXITSTATUS(wstat));
         }
      } else if (task.inft[i] == IFTobj || task.inft[i] == IFTar) {
      } else assert(!"not obj");
   }
   if (!ccopt.dbg.any) {
      atexit(cleantemps);
      signal(SIGINT, sigcleantemps);
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
      const char *cmd[] = {"/bin/env", prog, NULL};
      if (execv(*cmd, (char **)cmd)) {
         close(nulfd);
         error(NULL, "execv: %s", strerror(errno));
         exit(125);
      }
   }
   int wstat;
   waitpid(p, &wstat, 0);
   if (!WIFEXITED(wstat)) return 0;
   return WEXITSTATUS(wstat) < 125;
}

struct cmdargs { vec_of(const char *); };

static void
findlinkcmd(struct cmdargs *cmd)
{
   if (task.targ && (target.os != HOST_OS || target.arch != HOST_ARCH || target.abi != HOST_ABI)) {
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
      if (hasprog("zig")) {
         vpush(cmd, "zig");
         vpush(cmd, "cc");
         vpush(cmd, "-target");
         vpush(cmd, task.targ);
      } else {
         fatal(NULL, "cannot link to cross-compilation target: no appropiate toolchain installed");
      }
   } else {
      vpush(cmd, "cc");
   }
}

static int
dolink(void)
{
   const char *cmdbuf[100];
   pid_t p;
   int wstat;
   struct cmdargs cmd = VINIT(cmdbuf, countof(cmdbuf));

   vpush(&cmd, "/bin/env");
   findlinkcmd(&cmd);
   if (!strcmp(cmd.p[1], "zig")) {
      note(NULL, "using 'zig cc' as a cross-compiler");
   }
   if (task.outft == OFTdll) {
      vpush(&cmd, "-shared");
   } else if (task.outft == OFTexe) {
      vpush(&cmd, ccopt.pie ? "-pie" : "-no-pie");
   }
   vpushn(&cmd, task.linkargs.p, task.linkargs.n);
   vpush(&cmd, "-o");
   vpush(&cmd, task.out);
   assert(task.ninf > 0);
   for (int i = 0; i < task.ninf; ++i) {
      const char *o;
      switch (task.inft[i]) {
      case IFTc: o = tempobj[i]; break;
      case IFTobj: case IFTar:
         o = task.inf[i]; break;
      default: assert(!"link obj?");
      }
      vpush(&cmd, o);
   }
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
      if (execv(cmd.p[0], (char **)cmd.p)) {
         error(NULL, "execv: %s\n", strerror(errno));
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
   int fexecve(int fd, char *const argv[], char *const envp[]);
   fexecve(fd, task.runargs - 1, environ);
   error(NULL, "fexecv: %s\n", strerror(errno));
   return 1;
}

static int
driver(void)
{
   void cpp(struct wbuf *, const char *);
   if (task.verbose)
      efmt("# Target: %s\n", task.targ ? task.targ : "(host)");
   if (task.outft == OFTobj) {
      assert(task.ninf == 1);
      if (*task.inft != IFTc)
         fatal(NULL, "not a C source file: %s", task.inf[0]);
      return cc1(task.out, *task.inf);
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
      if (!task.out && task.ninf == 1)
         cpp(buf, task.inf[0]);
      else for (int i = 0; i < task.ninf; ++i) {
         pid_t p;
         int wstat;

         if ((p = fork()) < 0) {
            error(NULL, "fork(): %s\n", strerror(errno));
            ok = 0;
         } else if (p == 0) {
            cpp(buf, task.inf[i]);
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
      if (ccopt.dbg.any) return 0;
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
   if (!ccopt.dbg.any) objini(in, out);
   ccomp(in);
   if (!ccopt.dbg.any && !nerror) objfini();
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
      addinclpath(paths[i]);
}

static void
prihelp(void)
{
   pfmt("Usage: antcc [options] infile(s)...\n"
        "       antcc [options] -run infile [arguments...]\n"
        "       antcc [options] infile(s)... -run [arguments...]\n"
        "Options:\n"
        " -help   \tPrint this help message\n"
        " -std=<..> \tSet C standard (c89, c99, c11, c23)\n"
        " -pedantic \tWarnings for strict standards compliance\n"
        " -d{pamoilr} \tDebug print IR after {parse, abi, mem, opts, isel, live, rega}\n"
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
        " -O0    \tTurn off optimizations\n"
        " -O1    \tTurn on more optimizations\n"
        " -x[c|o] \tSpecify type of next input file (C, object)\n"
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
      fatal(NULL, "unsupported target: %s", task.targ ? task.targ : "(host)");

   return driver();
}

/* vim:set ts=3 sw=3 expandtab: */
