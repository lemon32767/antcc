#include "common.h"

extern struct objfile {
   const char *file;
   uchar *textbegin, *textend;
   uchar *code;
} objout;

void objini(const char *);
void objfini(void);

/* vim:set ts=3 sw=3 expandtab: */
