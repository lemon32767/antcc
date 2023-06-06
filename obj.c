#include "obj.h"
#include "common.h"

struct objfile objout;

void
objini(const char *file)
{
   enum { NTEXT = 4<<20 /* 4MiB */ };
   assert(!objout.file);
   objout.file = file;
   objout.code = objout.textbegin = mapzeros(NTEXT);
   objout.textend = objout.textbegin + NTEXT;
}

void
objfini(void)
{
   void *popen(char *, char *), pclose(void *);
   long fwrite(void *, size_t, size_t, void *);
   void *cmd = popen("ndisasm -b64 -", "w");
   ioflush(&bstderr); ioflush(&bstdout);
   fwrite(objout.textbegin, 1, objout.code - objout.textbegin, cmd);
   pclose(cmd);
}

/* vim:set ts=3 sw=3 expandtab: */
