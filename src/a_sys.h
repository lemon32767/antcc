#pragma once

#include "antcc.h"

const char *fileext(const char *);
char *withext(Arena **, const char *path, const char *ext);

bool isfile(const char *);
bool isdir(const char *);
bool mkdir_p(const char *);

enum procstdmode { PSIONUL = -10, PSIOCAPTURE };
typedef struct {
   union {
      struct {int in, out, err;};
      int _io[3];
   };
   const char *name;
   bool log, quiet, nopath;
} ProcSpawn;
typedef struct {
   bool ok;
   int pid;
   int wstat;
   const char *name;
   int _pipe[3][2];
} ProcIO;
ProcIO procspawn(const char **argv, const ProcSpawn *);
int procread(ProcIO *, char *, size_t, int f);
WriteBuf procwritebuf(ProcIO *, char *, size_t);
int procwait(ProcIO *);
int runproc(const char **argv, const ProcSpawn *);
void epricmd(const char **argv);
int checkwstat(int wstat, const char *thing);

/* vim:set ts=3 sw=3 expandtab: */
