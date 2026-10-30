#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* WCOREDUMP on glibc */
#include "a_sys.h"
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* "foo.bar" -> "bar"; ".dotfile" -> "" */
const char *
fileext(const char *path) {
   const char *dot = strrchr(path, '.');
   return dot ? dot+1 : "";
}

/* withext("x/y.c", "o") -> "y.o"; withext("f9", "s") -> "f9.s" */
char *
withext(Arena **arena, const char *path, const char *ext) {
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
   res = alloc(arena, len + 1 + (ext ? strlen(ext) + 1 : 0), 1);
   memcpy(res, file, len);
   if (ext) {
      res[len] = '.';
      memcpy(res + len + 1, ext, strlen(ext)+1);
   } else {
      res[len] = 0;
   }
   return res;
}

bool
isfile(const char *p) {
   struct stat st;
   return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

bool
isdir(const char *p) {
   struct stat st;
   return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

bool
mkdir_p(const char *p) {
   char buf[PATH_MAX];
   size_t n = 0;
   for (; *p; ++p, ++n) {
      if (n >= sizeof buf - 3) return 0;
      buf[n] = *p;
      if (*p != '/' || n == 0) continue;
      buf[n+1] = 0;
      if (mkdir(buf, 0700) != 0 && errno != EEXIST && errno != EISDIR) return 0;
   }
   buf[n] = 0;
   if (mkdir(buf, 0700) != 0 && errno != EEXIST && errno != EISDIR) return 0;
   struct stat st;
   if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
   return 1;
}

ProcIO
procspawn(const char **argv, const ProcSpawn *ps) {
   const char *name = ps && ps->name ? ps->name : *argv;
   ProcIO p = {.pid = 0, .wstat = -1, .name = name, ._pipe = {{-1,-1}, {-1,-1}, {-1,-1}}};
   bool q = ps && ps->quiet;

   if (ps && ps->log)
      epricmd(argv);

   for (int i = 0; i < 3; ++i) {
      int mode = ps ? ps->_io[i] : 0;
      if (mode == PSIOCAPTURE && pipe(p._pipe[i]) != 0) {
         if (!q)
            efmt("(spawn(%'s)) pipe(%d): %s\n", p.name, i, strerror(errno));
         goto Fail;
      }
   }

   if ((p.pid = fork()) < 0) {
      if (!q)
         efmt("(spawn(%'s)) fork: %s\n", p.name, strerror(errno));
      goto Fail;
   } else if (p.pid == 0) {
      for (int i = 0, fd; i < 3; ++i) {
         int io = ps ? ps->_io[i] : 0, nulfd;
         if (io == PSIONUL) {
            if ((nulfd = open("/dev/null", O_RDWR)) < 0) {
               if (!q) efmt("(spawn(%'s)) open(\"/dev/null\"): %s\n", p.name, strerror(errno));
               _exit(125);
            }
            fd = nulfd;
         } else if (io == PSIOCAPTURE) {
            fd = p._pipe[i][i == STDIN_FILENO ? 0 : 1];
         } else if (io > 0 && io != i) {
            fd = io;
         } else {
            continue;
         }
         if (dup2(fd, i) < 0) {
            if (!q) efmt("(spawn(%'s)) dup2: %s\n", p.name, strerror(errno));
            _exit(125);
         }
         if (fd != i)
            close(fd);
         if (io == PSIOCAPTURE)
            p._pipe[i][i == STDIN_FILENO ? 0 : 1] = -1;
      }
      for (int i = 0; i < 3; ++i)
         for (int j = 0; j < 2; ++j) {
            int fd = p._pipe[i][j];
            if (fd != -1 && fd > 2)
               close(fd);
         }
      (ps && ps->nopath ? execv : execvp)(name, (char **)argv);
      if (!q) efmt("(spawn(%'s)) exec: %s\n", p.name, strerror(errno));
      _exit(125);
   }
   for (int i = 0; i < 3; ++i) {
      int end = ps && ps->_io[i] == PSIOCAPTURE && i == STDIN_FILENO ? 0 : 1;
      if (p._pipe[i][end] != -1) {
         close(p._pipe[i][end]);
         p._pipe[i][end] = -1;
      }
   }
   return p;
Fail:
   for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 2; ++j)
         if (p._pipe[i][j] != -1)
            close(p._pipe[i][j]);
   return (ProcIO){.pid = -1, .wstat = -1, .name = name,
                   ._pipe = {{-1, -1}, {-1, -1}, {-1, -1}}};
}

int
procread(ProcIO *p, char *buf, size_t nbuf, int f) {
   assert(in_range(f, 0, 2));
   size_t n = 0;
   if (!nbuf) return 0;
   while (n < nbuf - 1) {
      ssize_t r = read(p->_pipe[f][0], buf + n, nbuf - 1 - n);
      if (r > 0) n += r;
      else if (r < 0 && errno == EINTR) continue;
      else break;
   }
   buf[n] = 0;
   return n;
}

WriteBuf
procwritebuf(ProcIO *p, char *buf, size_t cap) {
   int fd = p->_pipe[STDIN_FILENO][1];
   assert(fd != -1);
   return (WriteBuf)FDBUF(buf, cap, fd);
}

int
procwait(ProcIO *p) {
   p->wstat = -1;
   for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 2; ++j)
         if (p->_pipe[i][j] != -1) {
            close(p->_pipe[i][j]);
            p->_pipe[i][j] = -1;
         }
   if (p->pid < 0)
      return -1;
   int wst, r;
   while ((r = waitpid(p->pid, &wst, 0)) < 0 && errno == EINTR) ;
   if (r < 0) {
      efmt("waitpid(%y): %s\n", p->name, strerror(errno));
      p->ok = 0;
      return -1;
   }
   p->wstat = wst;
   int rc = checkwstat(wst, p->name);
   p->ok = rc == 0;
   return rc;
}
int
runproc(const char **argv, const ProcSpawn *ps) {
   ProcIO p = procspawn(argv, ps);
   return procwait(&p);
}

void
epricmd(const char **argv) {
   for (int i = 0; argv[i]; ++i)
      efmt("%s%s", i ? " " : "", argv[i]);
   efmt("\n");
}

int
checkwstat(int wstat, const char *thing) {
   if (WIFSIGNALED(wstat)) {
      efmt("%s: %s%s\n", thing, strsignal(WTERMSIG(wstat)),
            WCOREDUMP(wstat) ? " (core dumped)" : "");
      return 128 + WTERMSIG(wstat);
   }
   return WEXITSTATUS(wstat);
}

/* vim:set ts=3 sw=3 expandtab: */
