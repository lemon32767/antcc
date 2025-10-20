#include "common.h"
#include "obj/obj.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct option ccopt;
struct inclpaths *cinclpaths;

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

enum inft { IFTauto, IFTc, IFTasm, IFTobj };

static enum inft
ftdetect(const char *s)
{
   const char *ext = fileext(s);
   if (!strcmp(ext, "c")) return IFTc;
   if (!strcmp(ext, "o")) return IFTobj;
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
   res = alloc(&globarena, len + 1 + strlen(ext) + 1, 1);
   memcpy(res, file, len);
   res[len] = '.';
   memcpy(res + len + 1, ext, strlen(ext));
   return res;
}

static struct task {
   enum outft { OFTexe, OFTdll, OFTobj, OFTasm, OFTc } outft;
   const char *out;
   const char *targ;
   const char *inf[64];
   enum inft inft[64];
   vec_of(const char *) linkargs;
   int ninf;
   bool verbose;
} task;

static void prihelp(void);

static void
optparse(char **args)
{
   const char *arg, *x;
   enum inft ft = IFTauto;

   while ((arg = *++args)) {
      if (*arg++ != '-' || !*arg) {
         assert(task.ninf < arraylength(task.inf) && "too many infiles");
         task.inf[task.ninf] = arg[-1] != '-' ? arg-1 : "/dev/stdin";
         task.inft[task.ninf] = ft ? ft : ftdetect(arg-1);
         ++task.ninf;
         ft = IFTauto;
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
      } else if (*arg == 'd' && arg[1]) {
         /* see common.h§struct option */
         while (*++arg) switch (*arg | 32) {
         case 'p': ccopt.dbg.p = 1; break;
         case 'a': ccopt.dbg.a = 1; break;
         case 'i': ccopt.dbg.i = 1; break;
         case 'o': ccopt.dbg.o = 1; break;
         case 'l': ccopt.dbg.l = 1; break;
         case 'r': ccopt.dbg.r = 1; break;
         case 'm': ccopt.dbg.m = 1; break;
         default: warn(NULL, "-d: invalid debug flag %'c", *arg);
         }
      } else if (*arg == 'o') {
         if (arg[1]) task.out = arg+1;
         else if (args[1]) task.out = *++args;
         else goto Bad;
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
      } else if (*arg == 'l') {
         vpush(&task.linkargs, arg-1);
      } else if (!strcmp(arg, "v") || !strcmp(arg, "-verbose")) {
         task.verbose = 1;
      } else if (!strcmp(arg, "c")) {
         task.outft = OFTobj;
      } else if (!strcmp(arg, "E")) {
         task.outft = OFTc;
      } else Bad: warn(NULL, "invalid option: %'s", arg-1);
   }

   if (!task.ninf) fatal(NULL, "no input files");

   if (!task.out) {
      switch (task.outft) {
      case OFTdll:
      case OFTexe: task.out = "a.out"; break;
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
   bfmt(&fbuf, "%s/cc%x@%u@", tmpdir, id, id2++);
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

static const char *tempobj[arraylength(task.inf)];
static void
mktemps(void) {
   for (int i = 0; i < task.ninf; ++i)
      tempobj[i] = tempfile(task.inf[i], "o");
}
static void
cleantemps(void)
{
   for (int i = 0; i < task.ninf; ++i) {
      if (!tempobj[i]) break;
      unlink(tempobj[i]);
   }
}

static void
compileobjs(void)
{
   int wstat;
   pid_t p;

   if (!ccopt.dbg.any) mktemps();
   for (int i = 0; i < task.ninf; ++i) {
      flushstd();
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
   }
   if (!ccopt.dbg.any) atexit(cleantemps);
}

static int
dolink(void)
{
   static const char *cmdbuf[10];
   pid_t p;
   int wstat;
   vec_of(const char *) cmd = VINIT(cmdbuf, arraylength(cmdbuf));

   /* TODO don't depend on external c compiler, find lib and runtime paths and
    * invoke linker directly.. */

   vpush(&cmd, "/bin/gcc");
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
      vpush(&cmd, tempobj[i]);
   }
   if (task.verbose) {
      efmt("> ");
      for (int i = 0; i < cmd.n; ++i)
         efmt("%s ", cmd.p[i]);
      efmt("\n");
   }
   vpush(&cmd, NULL);
   flushstd();
   if ((p = fork()) < 0) {
      error(NULL, "fork(): %s\n", strerror(errno));
      exit(1);
   } else if (p == 0) {
      if (!execv(cmd.p[0], (char **)cmd.p)) {
         error(NULL, "execv(): %s\n", strerror(errno));
         exit(1);
      }
   }
   waitpid(p, &wstat, 0);
   if (!WIFEXITED(wstat)) return 127;
   return WEXITSTATUS(wstat);
}

static void
prihelp(void)
{
   pfmt("Usage: antcc [options] file...\n\n"
       "Options:\n"
       " -help   \tPrint this help message\n"
       " -std=<..> \tSet C standard\n"
       " -pedantic \tWarnings for strict standards compliance\n"
       " -d{pailrm} \tDebug options\n"
       " -o <file> \tPlace the output into <file>\n"
       " -v     \tVerbose output\n"
       " -c     \tEmit object file but do not link\n"
       " -fpie  \tEmit code for position independent executable\n"
       " -fpic  \tEmit position independent code\n"
   );
}

#include <fcntl.h> /* open */

static int
driver(void)
{
   void cpp(struct wbuf *, const char *);
   if (task.verbose)
      efmt("# Target: %s\n", task.targ);
   if (task.outft == OFTobj) {
      assert(task.ninf == 1);
      assert(*task.inft == IFTc && "nyi");
      return cc1(task.out, *task.inf);
   } else if (task.outft == OFTc) {
      static char tmp[1<<12];
      struct wbuf _buf = FDBUF(tmp, sizeof tmp, 1),
                  *buf = &_buf;
      bool ok = 1;
      if (task.out) {
         buf->fd = open(task.out, O_CREAT | O_TRUNC | O_WRONLY, 0777);
         if (buf->fd < 0) {
            error(NULL, "open(%'s): %s", task.out, strerror(errno));
            return 1;
         }
      }
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
      if (task.out) {
         close(buf->fd);
      }
      return ok ? 0 : 1;
   } else if (task.outft == OFTexe || task.outft == OFTdll) {
      compileobjs();
      if (ccopt.dbg.any) return 0;
      return dolink();
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
      "/usr/local/include",
      "/usr/include"
   };
   for (int i = 0; i < arraylength(paths); ++i) {
      struct inclpaths *p = alloc(&globarena, sizeof *cinclpaths, 0);
      p->next = cinclpaths;
      p->path = paths[i];
      cinclpaths = p;
   }
}

int
main(int argc, char **argv)
{
   atexit(flushstd);
   globarena->cap = sizeof(_arenamem.mem) - sizeof(struct arena);

   /* setup defaults */
   detectcolor();
   sysinclpaths();
   ccopt.cstd = STDC99;
   ccopt.pie = 1;

   /* parse cli ags */
   optparse(argv);

   /* global init */
   task.targ = task.targ ? task.targ : "amd64-sysv";
   targ_init(task.targ);

   return driver();
}

/* vim:set ts=3 sw=3 expandtab: */
