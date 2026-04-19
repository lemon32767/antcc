#include "antcc.h"
#include "version.h"
#include "hostconfig.h" /* run ./configure */
#include "obj.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

CCOption ccopt;
CInclPath *cinclpaths[5];
static CInclPath **cinclpath_tails[5];
static int nsysinclpaths;

static void
addinclpath(int ord, const char *path)
{
   CInclPath *p = alloc(&globarena, sizeof *p, 0);
   assert((uint)ord < countof(cinclpaths));
   p->path = path;
   p->next = NULL;
   if (cinclpaths[ord]) {
      *cinclpath_tails[ord] = p;
   } else {
      cinclpaths[ord] = p;
   }
   cinclpath_tails[ord] = &p->next;
   if (*path == '/')
      ++nsysinclpaths;
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

static bool
isinteger(const char *s)
{
   do
      if (!in_range(*s, '0', '9')) return 0;
   while (*++s);
   return 1;
}

static enum inft
ftdetect(const char *s)
{
   const char *ext = fileext(s);
   if (!strcmp(ext, "c")) return IFTc;
   if (!strcmp(ext, "o")) return IFTobj;
   if (!strcmp(ext, "a")) return IFTar;
   if (!strcmp(ext, "s")) return IFTasm;
   if (!strcmp(ext, "so") || isinteger(ext)) return IFTdll;
   warn(NULL, "assuming %'s is C source file", s);
   return IFTc;
}

static union {
   Arena a;
   char mem[sizeof(Arena) + (1<<10)];
} _arenamem;
Arena *globarena = &_arenamem.a;

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

typedef struct {
   enum inft ft;
   const char *path, *temp;
} InFile;
typedef struct Task {
   enum outft { OFTexe, OFTdll, OFTobj, OFTasm, OFTc } outft;
   const char *out;
   const char *targ;
   vec_of(InFile) inf;
   char **runargs;
   vec_of(const char *) linkargs;
   bool link_with_cc;
   bool verbose, run, syntaxonly;
} Task;
static InFile infilebuf[16];
static Task task = { .inf = VINIT(infilebuf, countof(infilebuf)) };

static void prihelp(void);

void cpp0define(const char *name, const char *body);
void cpp0undef(const char *name);

static void
predef(bool undef, const char *cmd)
{
   char buf[1024];
   const char *sep = strchr(cmd, '='),
              *body = sep ? sep+1 : "1";
   const char *name;
   if (sep) {
      uint n = sep - cmd;
      assert(n < sizeof buf - 1);
      memcpy(buf, cmd, n);
      buf[n] = 0;
      name = buf;
   } else {
      name = cmd;
   }
   if (undef) cpp0undef(name);
   else cpp0define(name, body);
}

/* needle in NUL-separated list of strings? */
static bool
stroneof(const char *needle, const char *haystack)
{
   for (const char *s = haystack; *s; s += strlen(s)+1) {
      if (!strcmp(s, needle))
         return 1;
   }
   return 0;
}

static void
optparse(char **args)
{
   const char *arg, *x;
   enum inft ft = IFTauto;

   while ((arg = *++args)) {
      if (*arg++ != '-' || !*arg) {
         vpush(&task.inf, ((InFile) {
            ft ? ft : ftdetect(arg-1),
            arg[-1] != '-' ? arg-1 : "/dev/stdin"
         }));
         vpush(&task.linkargs, NULL);
         ft = IFTauto;
         if (task.run) {
            task.runargs = args+1;
            return;
         }
         continue;
      }
      const char *incpath = NULL;
      int cinclord;
      if (stroneof(arg, "help\0h\0-help\0")) {
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
         if (*x == 'c') x += 1;
         else if (!memcmp(x, "gnu", 3)) x += 3;
         else goto UnkOption;

         if (!strcmp(x, "89") || !strcmp(x, "90")) ccopt.cstd = STDC89;
         else if (!strcmp(x, "99")) ccopt.cstd = STDC99;
         else if (!strcmp(x, "11") || !strcmp(x, "17")) ccopt.cstd = STDC11;
         else if (!strcmp(x, "2x")) ccopt.cstd = STDC23;
         else if (!strcmp(x, "23")) ccopt.cstd = STDC23;
         else goto UnkOption;
      } else if (!strcmp(arg, "pedantic")) {
         ccopt.pedant = 1;
      } else if (!strcmp(arg, "trigraphs")) {
         ccopt.trigraph = 1;
      } else if (*arg == 'd' && arg[1]) {
         /* see common.h§CCOption */
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
         if (stroneof(flag, "pie\0PIE\0")) ccopt.pie = set;
         else if (stroneof(flag, "pic\0PIC\0")) ccopt.pic = set;
         else {
            /* codegen flags unsupported */
            static const char badcgflags[] = "lto\0lto=full\0lto=thin\0";

            /* flags 'supported' by default, no need to warn as unrecognized */
            static const char okcgflags[] =
               "wrapv\0no-strict-aliasing\0no-jump-tables\0no-builtin\0signed-bitfields\0";

            if (stroneof(arg+1, badcgflags)) /* error here helps AC configure scripts */
               error(NULL, "unsupported command-line option: `%s`", arg-1);
            else if (!stroneof(arg+1, okcgflags))
               warn(NULL, "unsupported command-line option: `%s`", arg-1);
         }
      } else if (!strcmp(arg, "match=native") || !memcmp(arg, "mtune=", 6)) {
         /* ignore */
      } else if (stroneof(arg, "target\0-target\0")) {
         const char *s = *++args;
         if (!s) fatal(NULL, "missing target name");
         task.targ = s;
      } else if (!strcmp(arg, "pthread")) {
         cpp0define("_REENTRANT", NULL);
         vpush(&task.linkargs, "-lpthread");
      } else if (stroneof(arg, "shared\0static\0pie\0no-pie\0static-pie\0")) {
         /* XXX having some issues with linker commands for -shared */
         if (!strcmp(arg, "shared"))
            task.link_with_cc = 1;
         vpush(&task.linkargs, arg-1);
      } else if (!memcmp(arg, "Wl,", 3)) {
         if (task.link_with_cc) {
            vpush(&task.linkargs, arg-1);
         } else {
            for (char *larg = (char*)arg+3, *next; larg && *larg; larg = next) {
               next = strchr(larg, ',');
               if (next) {
                  *next = '\0';
                  ++next;
               }
               vpush(&task.linkargs, larg);
            }
         }
      } else if (strchr("zLlB", *arg)) {
         const char *x = arg[1] ? arg+1 : *++args;
         if (!x) fatal(NULL, "missing argument to `%S`", arg-1, 2);
         if (*arg == 'z') {
            vpush(&task.linkargs, "-z");
            vpush(&task.linkargs, x);
         } else {
            if (arg[1]) vpush(&task.linkargs, arg-1);
            else {
               char *temp = alloc(&globarena, 2 + strlen(x) + 1, 1);
               temp[0] = '-', temp[1] = *arg;
               strcpy(temp+2, x);
               vpush(&task.linkargs, temp);
            }
         }
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
         else goto UnkOption;
      } else if (*arg == 'D' || *arg == 'U') {
         const char *def = arg[1] ? arg+1 : *++args;
         if (!def) fatal(NULL, "macro name missing after `-%c`", *arg);
         predef(*arg == 'U', def);
      } else if (*arg == 'O') {
         /* TODO optimization level */
      } else if (*arg == 'I' || !strcmp(arg, "-include-directory")) {
         cinclord = CINCL_I;
         if (*arg == 'I' && arg[1]) incpath = arg+1;
         else incpath = *++args;
      CIncl:
         if (!incpath) fatal(NULL, "missing path after `%s`", arg-1);
         addinclpath(cinclord, incpath);
      } else if (!memcmp(arg, "iquote", 6)) {
         cinclord = CINCL_iquote;
         incpath = arg[6] ? arg+6 : *++args;
         goto CIncl;
      } else if (!memcmp(arg, "isystem", 7)) {
         cinclord = CINCL_isystem;
         incpath = arg[7] ? arg+7 : *++args;
         goto CIncl;
      } else if (!memcmp(arg, "idirafter", 9)) {
         cinclord = CINCL_idirafter;
         incpath = arg[9] ? arg+9 : *++args;
         goto CIncl;
      } else if (!strcmp(arg, "nostdinc") || !strcmp(arg, "-no-standard-includes")) {
         cinclpaths[CINCLsys] = NULL;
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
      } else UnkOption: error(NULL, "unrecognized command-line option: %'s", arg-1);
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
   WriteBuf fbuf = MEMBUF(sbuf, sizeof sbuf);

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

typedef vec_of(const char *) CmdArgs;

static void
findlinkcmd(CmdArgs *cmd)
{
   if (task.targ && iscrosscc()) {
      task.link_with_cc = 1;
      /* try to find a cross compiling toolchain, e.g. aarch64-linux-gnu-gcc */
      static const char *ccs[] = {"cc", "gcc", "clang"};
      char cross[1024];
      for (int i = 0; i < countof(ccs); ++i) {
         WriteBuf wbuf = MEMBUF(cross, sizeof cross);
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
   } else if (task.link_with_cc) {
      vpush(cmd, HOST_CC);
   } else {
      vpush(cmd, HOST_LD);
      if (*host_linkcmd) vpushn(cmd, host_linkcmd, countof(host_linkcmd));
      if (ccopt.pie) {
         if (*host_ldstartfiles_pie)
            vpushn(cmd, host_ldstartfiles_pie, countof(host_ldstartfiles_pie));
      } else {
         if (*host_ldstartfiles)
            vpushn(cmd, host_ldstartfiles, countof(host_ldstartfiles));
      }
   }
}

static int
dolink(void)
{
   const char *cmdbuf[100];
   pid_t p;
   int wstat;
   CmdArgs cmd = VINIT(cmdbuf, countof(cmdbuf));

   findlinkcmd(&cmd);
   if (!strcmp(cmd.p[0], "zig")) {
      note(DGNOTE, NULL, "using 'zig cc' as a cross-compiler");
   }
   if (task.outft == OFTdll) {
      vpush(&cmd, "-shared");
   } else if (task.outft == OFTexe) {
      vpush(&cmd, ccopt.pie ? "-pie" : "-no-pie");
   }
   vpush(&cmd, "-o");
   vpush(&cmd, task.out);
   assert(task.inf.n > 0);

   for (int l = 0, i = 0; l < task.linkargs.n; ++l) {
      const char *a = task.linkargs.p[l];
      if (!a) {
         switch (task.inf.p[i].ft) {
         case IFTc: a = task.inf.p[i].temp; break;
         case IFTobj: case IFTar: case IFTdll:
            a = task.inf.p[i].path; break;
         default: assert(!"link obj?");
         }
         ++i;
      }
      vpush(&cmd, a);
   }
   if (!task.link_with_cc) {
      if (ccopt.pie) {
         if (*host_ldstartfiles_pie)
            vpushn(&cmd, host_ldendfiles_pie, countof(host_ldendfiles_pie));
      } else {
         if (*host_ldendfiles)
            vpushn(&cmd, host_ldendfiles, countof(host_ldendfiles));
      }
   }
   if (task.verbose) {
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
      efmt("exec %s", task.out);
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
   void cpp(WriteBuf *, const char *);
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
      WriteBuf _buf = {0}, *buf = &bstdout;
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
   if (!task.syntaxonly && !nerror) objfini(!ccopt.dbg.any);
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

static const TargTriple host_targ = { HOST_ARCH, HOST_OS, HOST_ABI };

int
main(int argc, char **argv)
{
   globarena->cap = sizeof(_arenamem.mem) - sizeof(Arena);

   ioinit();
   /* setup defaults */
   detectcolor();
   sysinclpaths();
   ccopt.cstd = STDC11;
   ccopt.pie = 1;
   ccopt.dbgout = &bstdout;
   if (getenv("ANTCC_VERBOSE")) {
      task.verbose = 1;
   }
   task.link_with_cc = HOST_LINK_WITH_CC;

   /* parse cli ags */
   if (argc == 1) {
      prihelp();
      return 1;
   }
   int nincl0 = nsysinclpaths;
   optparse(argv);

   /* global init */
   if (!targ_init(task.targ, &host_targ) || !target.arch) {
      fatal(NULL, "unsupported target: %s", task.targ ? task.targ : HOST_TRIPLE);
   }
   if (iscrosscc() && nsysinclpaths == nincl0) {
      /* try '/usr/<target>/include' */
      struct stat st;
      char path[4096];
      int n = bfmt(&(WriteBuf)MEMBUF(path,sizeof path), "/usr/%s/include/%c", task.targ, 0);
      if (n < sizeof path
       && stat(path, &st) == 0) {
         path[n-2] = '\0';
         addinclpath(CINCL_isystem, alloccopy(&globarena, path, n, 1));
         note(0, NULL, "found cross compiler include path %'s", path);
      } else {
         warn(NULL,
               "defaulting to host include paths while cross compiling for %s might not work",
               task.targ);
      }
   }

   for (const char *const *p = host_predefs; *p; ++p)
      predef(0, *p);

   return driver();
}

/* vim:set ts=3 sw=3 expandtab: */
