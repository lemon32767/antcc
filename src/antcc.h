#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#ifdef __clang__ /* stop linter from complaining about "unused" inline functions */
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define bool _Bool
typedef unsigned char uchar;
typedef   signed char schar;
typedef unsigned short ushort;
typedef unsigned long long u64int;
typedef   signed long long s64int;
typedef unsigned uint;

#if __STDC_VERSION__ >= 202311L
#define NORETURN [[noreturn]]
#elif __STDC_VERSION__ >= 201112L
#define NORETURN _Noreturn
#else
#define NORETURN
#endif

#ifdef __has_builtin
#define HAS_BUILTIN(b) __has_builtin(__builtin_##b)
#else
#define HAS_BUILTIN(_) 0
#endif

#define static_assert(x) _Static_assert(x, #x)
#define in_range(x, Lo, Hi) ((uint) (x) - (Lo) <= (Hi) - (Lo)) /* lo <= x <= hi; lo > 0, hi > 0 */
#define alignup(x, A) (((x) + ((A) - 1)) & -(A))
#define countof(a) (sizeof(a) / sizeof 0[a])

void _assertfmt(const char *file, int line, const char *func, const char *expr);
#if HAS_BUILTIN(trap)
#define assert(x) (!(x) ? _assertfmt(__FILE__,__LINE__,__func__,#x), __builtin_trap() : (void)0)
#else
#define assert(x) (void)(!(x) ? _assertfmt(__FILE__,__LINE__,__func__,#x), *(volatile int *)0 : 0)
#endif

#if HAS_BUILTIN(popcountll)
#define popcnt(x) __builtin_popcountll(x)
#else
static inline uint
popcnt(u64int x) {
   uint n = 0;
   while (x) n += x&1, x >>= 1;
   return n;
}
#endif

static inline bool
ispo2(u64int x) {
   return (x != 0) & ((x & (x - 1)) == 0);
}

#if HAS_BUILTIN(ctzll)
#define ilog2(x) __builtin_ctzll(x)
#else
static inline uint
ilog2(u64int x) { /* assumes x is a power of 2 */
   uint n = 0;
   while (x >>= 1) ++n;
   return n;
}
#endif

#if HAS_BUILTIN(ctzll)
#define lowestsetbit(x) __builtin_ctzll(x)
#else
static inline uint
lowestsetbit(u64int x)
{
   int i = 0;
   for (u64int mask = 1;; ++i, mask <<= 1)
      if (x & mask)
         break;
   return i;
}
#endif

#define aisprint(c) in_range(c, ' ', '~')
#define aisdigit(c) in_range(c, '0', '9')
#define aisodigit(c) in_range(c, '0', '7')
#define aisalpha(c) in_range((c)|0x20, 'a', 'z')
static inline bool aisspace(int c) { return c == ' ' || in_range(c, '\t', '\r'); }
static inline bool aisxdigit(int c) { return aisdigit(c) || in_range(c|0x20, 'a', 'f'); }

/******************/
/* COMPILER STATE */
/******************/

enum cstd {
   STDC89,
   STDC99,
   STDC11,
   STDC23,
};
typedef struct CCOption {
   enum cstd cstd;
   bool pedant;
   bool trigraph;
   bool nocolor;
   bool pie, pic;
   bool werror;
   bool wnone;
   enum optz {
      OPT0 = -1,
      OPT1 = 1,
      OPT2 = 2,
   } o;
   union {
      struct {
         bool p : 1, /* after parsing */
              a : 1, /* after abi0 */
              m : 1, /* after mem */
              y : 1, /* after inline */
              o : 1, /* after optimizations */
              s : 1, /* after stack */
              i : 1, /* after isel */
              l : 1, /* after liveness fixup */
              r : 1; /* after regalloc */
      };
      uint any;
   } dbg;
   struct WriteBuf *dbgout;
} CCOption;
extern CCOption ccopt;

typedef struct CInclPath {
   struct CInclPath *next;
   const char *path;
} CInclPath;
enum { /* GCC include directory search order: https://gcc.gnu.org/onlinedocs/gcc/Directory-Options.html#Options-for-Directory-Search */
   CINCL_iquote,
   CINCL_I,
   CINCL_isystem,
   CINCLsys,
   CINCL_idirafter,
};
extern CInclPath *cinclpaths[5];

/** c_lex.c **/
void cpp0define(const char *name, const char *body);
void cpp0undef(const char *name);

/**********/
/* Target */
/**********/

typedef struct TargTriple {
   enum mcarch { ISxxx, ISx86_64, ISaarch64 } arch;
   enum mcos { OSunknown, OSlinux, OSopenbsd } os;
   enum mcabi { ABInone, ABIgnu, ABImusl } abi;
} TargTriple;
extern TargTriple target;
enum objkind { OBJELF };
extern const struct MCTarg *mctarg;
bool targ_init(const char *, const TargTriple *dfault);

/*********/
/** MEM **/
/*********/

/* libc *alloc wrappers */
void *xmalloc(size_t);
void *xcalloc(size_t);
void *xrealloc(void *, size_t);
void free(void *);

/* string interning */
typedef const struct {char c;} *internstr;
internstr intern_(const char *, uint len);
#define intern(s) intern_(s, 0)

/* growable buffer that stores its capacity in the allocated memory */
#define xbnew_(n) (void *)(1 + (size_t *)xcalloc(sizeof(size_t) + (n)))
#define xbcap_(p) ((size_t *)(p))[-1]
static inline void
xbgrow_(void **p, size_t n)
{
   if (!n) return;
   if (!*p) { *p = xbnew_(n); xbcap_(*p) = n; assert(n>0); }
   else if (xbcap_(*p) < n) {
      size_t k = xbcap_(*p);
      do k *= 2; while (k < n);
      *p = 1 + (size_t *)xrealloc(&xbcap_(*(p)), sizeof(size_t) + k);
      xbcap_(*p) = k;
   };
}
#define xbgrow(p, n) xbgrow_((void **)(p), (n) * sizeof**(p))
#define xbpush(p, n, x) (xbgrow(p, (*(n) + 1)), (*(p))[(*(n))++] = (x))
#define xbfree(p) ((p) ? free(&xbcap_(p)) : (void)0)
#define xbcap(p) ((p) ? xbcap_(p) / sizeof*(p) : 0)
#define xbgrowz(p, n) do {                       \
   size_t tmp = *(p) ? xbcap_(*(p)) : 0;         \
   xbgrow(p, n);                                 \
   memset((char*)*(p)+tmp, 0, xbcap_(*(p))-tmp); \
} while (0)


/** arenas **/
typedef struct Arena {
   uint cap : 31,
        dyn : 1;
   uint n;
   struct Arena *prev;
   uchar mem[];
} Arena;

extern Arena *globarena;

Arena *newarena(uint chunksiz);
void *alloc(Arena **, uint siz, uint align);
void *allocz(Arena **, uint siz, uint align);
static inline void *
alloccopy(Arena **arena, const void *src, uint siz, uint align)
{
   if (!siz) return NULL;
   return memcpy(alloc(arena, siz, align), src, siz);
}
void freearena(Arena **);

/** bitset (u_bits.h) **/
typedef struct { size_t u; } BitSet;
enum { BSNBIT = 8 * sizeof(BitSet) };
#define BSSIZE(nbit) ((nbit)/BSNBIT + ((nbit)%BSNBIT != 0))

/** vec **/
struct vecbase { void *p; uint n; uint cap : 31, dyn : 1; };
#define vec_of(T) union {    \
   struct { T *p; uint n; }; \
   struct vecbase _vb;       \
}
void vinit_(struct vecbase *, void *inlbuf, uint cap, uint siz);
void vpush_(struct vecbase *, uint siz);
void *vpushn_(struct vecbase *, uint siz, const void *dat, uint ndat);
void vresize_(struct vecbase *, uint siz, uint N);
#define VINIT(inlbuf, Cap) { ._vb.p = (inlbuf), ._vb.cap = (Cap) }
#define vfree(v) ((v)->_vb.dyn ? free((v)->p) : (void)0, memset((v), 0, sizeof*(v)))
#define vinit(v, inlbuf, Cap) (vfree(v), vinit_(&(v)->_vb, inlbuf, (Cap), sizeof *(v)->p))
#define vpush(v, x) (vpush_(&(v)->_vb, sizeof *(v)->p), (v)->p[(v)->n++] = (x))
#define vpushn(v, xs, N) vpushn_(&(v)->_vb, sizeof *(v)->p, xs, N)
#define vresize(v, N) vresize_(&(v)->_vb, sizeof *(v)->p, N)

/** map of non-null ptr -> T **/
#define pmap_of(T) struct { T *v; int tmp; struct pmapbase mb; }
struct pmapbase { void **k; uint n, N; };

void pmap_init_(struct pmapbase *, void **v, uint vsiz, uint N);
int pmap_get_(struct pmapbase *, const void *k);
int pmap_set_(struct pmapbase *, void **v, uint vsiz, const void *k);
void pmap_del_(struct pmapbase *, const void *k);
extern char pmap_tombstone_[];
#define pmap_free(m) (free((m)->mb.k), memset((m), 0, sizeof *(m)))
#define pmap_init(m, N) (pmap_free(m), pmap_init_(&(m)->mb, (void **)&(m)->v, sizeof*(m)->v, (N)))
#define pmap_get(m, k) (((m)->tmp = pmap_get_(&(m)->mb, k)) < 0 ? NULL : &(m)->v[(m)->tmp])
#define pmap_set(m, k, x) ((m)->tmp = pmap_set_(&(m)->mb, (void **)&(m)->v, sizeof*(m)->v, k), \
                           (m)->v[(m)->tmp] = (x))
#define pmap_del(m, k) pmap_del_(&(m)->mb, k)
#define pmap_each(m,kx,pvx) \
   for (size_t _i = 0; _i < (m)->mb.N && ((kx) = (m)->mb.k[_i], (pvx) = &(m)->v[_i], 1); ++_i) \
      if (kx && kx != (void*)pmap_tombstone_)

/********/
/** IO **/
/********/

typedef struct WriteBuf {
   union {
      struct {
         char *buf;
         uint cap;
         uint len;
         int fd;
      };
      void *_fp;
   };
   bool err;
   bool isfp;
} WriteBuf;

/* read-only file mapping that is at least 1 page larger than the real file
 * so it's legal to read a few bytes beyond it to avoid some bounds checks */
typedef struct MemFile {
   const uchar *p;
   uint n;
   bool statik;
} MemFile;

#define MEMBUF(buf_, cap_) { .buf = (buf_), .cap = (cap_), .fd = -1 }
#define FDBUF(buf_, cap_, fd_)  { .buf = (buf_), .cap = (cap_), .fd = (fd_) }
extern WriteBuf bstdout, bstderr;
void ioinit(void);
void iowrite(WriteBuf *, const void *src, int n);
void ioputc(WriteBuf *, uchar);
void ioflush(WriteBuf *);
int vbfmt(WriteBuf *, const char *, va_list ap);
int bfmt(WriteBuf *, const char *, ...);
#define pfmt(...) bfmt(&bstdout, __VA_ARGS__)
#define efmt(...) bfmt(&bstderr, __VA_ARGS__)
MemFile mapopen(const char **err, const char *path);
void mapclose(MemFile *);
void *mapzeros(uint);
int munmap(void *, size_t);
int getpredeffile(MemFile **, const char *name);
int openfile(const char **err, MemFile **, const char *path);
const char *getfilename(int id, uint atoff);
MemFile *getfile(int id);
void addfileline(int id, uint off);
void setfileline(int id, uint off, int line, const char *file);
const char *getfilepos(int *line, int *col, int id, uint off);
bool isoncefile(int id, internstr *guard);
void markfileonce(int id, internstr guard);
void markfileseen(int id);
bool isfileseen(int id);
void closefile(int id);

#define SPANFILEBITS 10
typedef struct {
   uint off,
        len  : 32-SPANFILEBITS,
        file : SPANFILEBITS;
} Span0;

typedef struct {
   Span0 sl, /* original source location */
         ex; /* the location after #include/macro expansion */
} Span;

enum diagkind { DGERROR, DGWARN, DGNOTE, };
void vdiag(const Span *, enum diagkind, const char *, va_list);
NORETURN void fatal(const Span *, const char *, ...);
bool error(const Span *, const char *, ...);
bool warn(const Span *, const char *, ...);
void note(const Span *, const char *, ...);
ushort *utf8to16(uint *ulen, Arena **, const uchar *s, size_t len);
uint *utf8to32(uint *ulen, Arena **, const uchar *s, size_t len);
int utf8enc(char out[4], uint cp);

/* vim:set ts=3 sw=3 expandtab: */
