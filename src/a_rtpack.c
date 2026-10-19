#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "z_miniz.h"
#undef assert // miniz includes <assert.h>
#undef static_assert
#include "antcc.h"
#include "version.h"
#include "obj.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void ccompbuf(const char *name, const char *data, uint size);

static Arena *arena;

static bool
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

static const char *
cacheroot(void) {
   const char *d = getenv("ANTCC_CACHE_DIR");
   if (d && *d) return d;
   const char *home = getenv("HOME");
   if (!home || !*home) return NULL;
   return afmt(&arena, "%s/.cache/antcc", home);
}

static bool
exists(const char *path) {
   struct stat st;
   return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static const char *
packdirpath(const char *root, const char *ver, const char *targ, const char *hash) {
   if (!root) return NULL;
   int nhash = (nhash = strlen(hash)) > 20 ? 20 : nhash;
   return afmt(&arena, "%s/%s/%s/%S", root, ver, targ, hash, nhash);
}

static int
rtcc1(const char *name, const char *out, const char *data, uint size) {
   extern int nerror;

   vrbfmt("rt cc1(in: %'s, out: %'s)\n", name, out);
   /* compilation must be deterministic here and not depend on invoked compiler flags */
   memset(&ccopt, 0, sizeof ccopt);
   ccopt.cstd = STDC11;
   ccopt.pic = 1;
   ccopt.o = OPT2;

   objini(name, out);
   ccompbuf(name, data, size);
   if (nerror) return 1;
   objfini(1);
   return 0;
}

static int
run(char *const argv[]) {
   pid_t p = fork();
   if (p < 0) return -1;
   if (p == 0) {
      execvp(argv[0], argv);
      _exit(127);
   }
   int st;
   if (waitpid(p, &st, 0) < 0) return -1;
   return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static bool
ar(const char *out, char **objs, int nobj) {
   int ncmd = 3 + nobj + 1;
   const char **cmd = alloc(&arena, ncmd * sizeof *cmd, 0);

   cmd[0] = "ar", cmd[1] = "-rcs", cmd[2] = out;
   for (int i = 0; i < nobj; ++i)
      cmd[3 + i] = objs[i];
   cmd[ncmd-1] = NULL;
   if (ccopt.verbose) for (int i = 0; i < ncmd-1; ++i) {
      efmt("%s%s", i?" ":"", cmd[i]);
   }
   vrbfmt("\n");
   return run((char **)cmd) == 0;
}

static bool
compileaziplib(const char *path, mz_zip_archive *zip, const char **meta) {
   int metafd = -1;
   DEF_SVEC(char *, 256, objs);
   char *tmp = NULL, *databuf = NULL;
   bool ok = 0;

   /* dir = dirname(path) */
   const char *slash = strrchr(path, '/');
   char *dir;
   if (!slash) dir = ".";
   else {
      size_t ndir = slash - path;
      dir = alloccopy(&arena, path, ndir+1, 1);
      dir[ndir] = 0;
   }

   /* acquire lock */
   char *metapath = afmt(&arena, "%s/meta", dir);
   if ((metafd = open(metapath, O_CREAT|O_RDWR, 0600)) < 0)
      return 0;
   fcntl(metafd, F_SETFD, FD_CLOEXEC);
   if (fcntl(metafd, F_SETLKW, &(struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET}) != 0)
      goto End;
   if (exists(path)) { /* another process may have built it while we waited for the lock */
      vrbfmt(" # found @ %s\n", path);
      ok = 1;
      goto End;
   }
   vrbfmt(" # begin lock %s\n", metapath);
   ftruncate(metafd, 0);

   /* write metadata */
   char metabuf[200];
   WriteBuf metawbuf = FDBUF(metabuf, sizeof metabuf, metafd);
   for (int i = 0; meta[i]; ++i) {
      if (i) ioputc(&metawbuf, '\n');
      bfmt(&metawbuf, "%s", meta[i]);
   }
   ioflush(&metawbuf);

   /* use a temp so other processes don't take the .a mid write? */
   tmp = afmt(&arena, "%s/.libantrt1.a.tmp", dir);
   unlink(tmp);

   /* compile each obj in zip */
   uint nfiles = mz_zip_reader_get_num_files(zip);
   for (uint i = 0; i < nfiles; ++i) {
      mz_zip_archive_file_stat st;
      if (!mz_zip_reader_file_stat(zip, i, &st)) {
         warn(NULL, "unzip: %s", mz_zip_get_error_string(mz_zip_get_last_error(zip)));
         goto End;
      }
      if (mz_zip_reader_is_file_a_directory(zip, i))
         continue;

      const char *name = strrchr(st.m_filename, '/');
      name = name ? name + 1 : st.m_filename;
      const char *dot = strrchr(name, '.');
      int namelen = dot ? dot - name : strlen(name);
      char *out = afmt(&arena, "%s/%S.o", dir, name, namelen);

      assert(st.m_uncomp_size < 1<<20 && "?");
      uint size = st.m_uncomp_size;
      const uint pad = 8;
      databuf = databuf ? xrealloc(databuf, size + pad) : xmalloc(size + pad);
      if (!mz_zip_reader_extract_to_mem(zip, i, databuf, size, 0)) {
         warn(NULL, "unzip: %s", mz_zip_get_error_string(mz_zip_get_last_error(zip)));
         goto End;
      }
      memset(databuf + size, 0, pad);

      ioflush(&bstdout), ioflush(&bstderr);
      pid_t p = fork();
      if (p < 0) {
         warn(NULL, "fork: %s", strerror(errno));
         goto End;
      } else if (p == 0) {
         _exit(rtcc1(st.m_filename, out, databuf, size));
      }
      int wst;
      if (waitpid(p, &wst, 0) < 0 || !WIFEXITED(wst) || WEXITSTATUS(wst) != 0) {
         warn(NULL, "rtpack: failed to compile %'s", st.m_filename);
         goto End;
      }

      vpush(&objs, out);
   }
   assert(objs.n > 0);

   /* make static lib */
   if (!ar(tmp, objs.p, objs.n)) {
      warn(NULL, "rtpack: ar failed");
      goto End;
   }

   /* atomic move tmp .a to dest path */
   if (rename(tmp, path) != 0) {
      warn(NULL, "rtpack: rename: %s", strerror(errno));
   } else {
      vrbfmt(" # %s OK\n", path);
      ok = 1;
   }
End:
   free(databuf);
   if (!ok && tmp)
      unlink(tmp);
   for (int i = 0; i < objs.n; ++i)
      unlink(objs.p[i]);
   vfree(&objs);
   if (metafd >= 0) {
      fcntl(metafd, F_SETLKW, &(struct flock){.l_type = F_UNLCK, .l_whence = SEEK_SET});
      close(metafd);
   }
   return ok;
}

const char *
rtpack_getlibantrt1(void) {
   extern unsigned pack_libantrt1_size;
   extern const char pack_libantrt1_data[],
                     pack_libantrt1_meta[],
                     pack_libantrt1_hash[];

   enum { N = 8<<10 };
   union {Arena a; char mem[sizeof(Arena)+N];} amem;
   *(arena = &amem.a) = (Arena){.cap = N};

   const char *root = cacheroot(), *dir = NULL, *path = NULL, *targ;
   if (!root)
      goto End;
   targ = targ_canon_triple();
   const char *shortver = afmt(&arena, "%d.%d.x", ANTCC_VERSION_MAJOR,ANTCC_VERSION_MINOR);
   dir = packdirpath(root, shortver, targ, pack_libantrt1_hash);
   vrbfmt("rt cache dir: %s\n", dir);
   path = afmt(&globarena, "%s/libantrt1.a", dir);
   if (exists(path)) {
      vrbfmt(" # found @ %s\n", path);
      goto End;
   }

   if (!mkdir_p(dir)) {
      warn(NULL, "runtime pack: cannot write to %'s, set ANTCC_CACHE_DIR", dir);
      path = NULL;
      goto End;
   }

   mz_zip_archive zip = {0};
   if (!mz_zip_reader_init_mem(&zip, pack_libantrt1_data, pack_libantrt1_size, 0)) {
      warn(NULL, "runtime pack: %s", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
      path = NULL;
      goto End;
   }
   const char *meta[] = {ANTCC_VERSION_STR, targ, pack_libantrt1_meta, NULL};
   if (!compileaziplib(path, &zip, meta))
      path = NULL;
   mz_zip_reader_end(&zip);
End:
   freearena(&arena);
   return path;
}

/* vim:set ts=3 sw=3 expandtab: */
