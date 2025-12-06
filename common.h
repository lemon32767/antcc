#ifndef COMMON_H_
#define COMMON_H_

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
typedef unsigned long long uvlong;
typedef   signed long long vlong;
typedef unsigned uint;


#ifdef __has_builtin
#define HAS_BUILTIN(b) __has_builtin(__builtin_##b)
#else
#define HAS_BUILTIN(_) 0
#endif
#define static_assert(x) _Static_assert(x, #x)
#define in_range(x, Lo, Hi) ((uint) (x) - (Lo) <= (Hi) - (Lo)) /* lo <= x <= hi; lo > 0, hi > 0 */
#define alignup(x, A) (((x) + ((A) - 1)) & -(A))
#define arraylength(a) (sizeof(a) / sizeof 0[a])

enum { SPANFILEBITS = 10 };
struct span {
   struct span0 {
      uint off;
      uint len  : 32-SPANFILEBITS,
           file : SPANFILEBITS;
   } sl, /* original source location */
     ex; /* the location after #include/macro expansion */
};

void _assertfmt(const char *file, int line, const char *func, const char *expr);
#if HAS_BUILTIN(trap)
#define assert(x) (!(x) ? _assertfmt(__FILE__,__LINE__,__func__,#x), __builtin_trap() : (void)0)
#else
#define assert(x) (void)(!(x) ? _assertfmt(__FILE__,__LINE__,__func__,#x), *(volatile int *)0 : 0)
#endif

static inline uint
hashs(uint h, const char *s)
{
   while (*s) h = (uchar)*s++ + h*65599;
   return h;
}
static inline uint
hashb(uint h, const void *d, uint n)
{

   const uchar *b = d;
   while (n--) h = *b++ + h*65599;
   return h;
}
static inline uint
ptrhash(const void *p) {
   return (uint)(size_t)p * 2654435761u;
}
static inline uint
popcnt(uvlong x) {
#if HAS_BUILTIN(popcountll)
   return __builtin_popcountll(x);
#else
   uint n = 0;
   while (x) n += x&1, x >>= 1;
   return n;
#endif
}
static inline bool
ispo2(uvlong x) {
   return (x != 0) & ((x & (x - 1)) == 0);
}
static inline uint
ilog2(uvlong x) { /* assumes x is a power of 2 */
#if HAS_BUILTIN(ctz)
   return __builtin_ctz(x);
#else
   uint n = 0;
   while (x >>= 1) ++n;
   return n;
#endif
}
static inline uint
lowestsetbit(uvlong x)
{
#if HAS_BUILTIN(ctz)
   return __builtin_ctzll(x);
#else
   int i = 0;
   for (uvlong mask = 1;; ++i, mask <<= 1)
      if (x & mask)
         break;
   return i;
#endif
}

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
struct option {
   enum cstd cstd;
   bool pedant;
   bool trigraph;
   bool nocolor;
   bool pie, pic;
   union {
      struct {
         bool p : 1, /* after parsing */
              a : 1, /* after abi0 */
              m : 1, /* after mem */
              o : 1, /* after optimizations */
              i : 1, /* after isel */
              l : 1, /* after liveness fixup */
              r : 1; /* after regalloc */
      };
      uchar any;
   } dbg;
   struct wbuf *dbgout;
};
extern struct option ccopt;
extern struct inclpaths {
   struct inclpaths *next;
   const char *path;
} *cinclpaths;

/**********/
/* Target */
/**********/

enum mcisa { ISamd64 };
extern const struct mctarg *mctarg;
extern enum mcisa targ_mcisa;
void targ_init(const char *);

/*********/
/** MEM **/
/*********/

/* libc *alloc wrappers */
void *xmalloc(size_t n, const char *);
void *xcalloc(size_t n, const char *);
void *xrealloc(void *, size_t n, const char *);
void free(void *);
#define xmalloc(n) xmalloc(n, __func__)
#define xcalloc(n) xcalloc(n, __func__)
#define xrealloc(p,n) xrealloc(p, n, __func__)

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
      assert(k > 0);
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
struct arena {
   uint cap : 31,
        dyn : 1;
   uint n;
   struct arena *prev;
   uchar mem[];
};

extern struct arena *globarena;

struct arena *newarena(uint chunksiz);
void *alloc(struct arena **, uint siz, uint align);
void *allocz(struct arena **, uint siz, uint align);
static inline void *
alloccopy(struct arena **arena, const void *src, uint siz, uint align)
{
   return memcpy(alloc(arena, siz, align), src, siz);
}
void freearena(struct arena **);

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

/** map of short -> T **/
#define imap_of(T) struct { T *v; int tmp; struct imapbase mb; }
struct imapbase { short *k; struct bitset *bs; uint n, N; };

void imap_init_(struct imapbase *, void **v, uint vsiz, uint N);
int imap_get_(struct imapbase *, short k);
int imap_set_(struct imapbase *, void **v, uint vsiz, short k);
#define imap_free(m) (free((m)->mb.k), memset((m), 0, sizeof *(m)))
#define imap_init(m, N) (imap_free(m), imap_init_(&(m)->mb, (void **)&(m)->v, sizeof*(m)->v, (N)))
#define imap_clear(m) ((m)->mb.bs ? bszero((m)->mb.bs, BSSIZE((m)->mb.N)) : (void)0, \
                       (m)->mb.n = 0)
#define imap_get(m, k) ((m)->tmp = imap_get_(&(m)->mb, k), (m)->tmp < 0 ? NULL : &(m)->v[(m)->tmp])
#define imap_set(m, k, ...) ((m)->tmp = imap_set_(&(m)->mb, (void **)&(m)->v, sizeof*(m)->v, k), \
                           (m)->v[(m)->tmp] = (__VA_ARGS__), &(m)->v[(m)->tmp])
#define imap_each(m,kx,pvx) \
   for (int _i = 0; _i < (m)->mb.N && ((kx) = (m)->mb.k[_i], (pvx) = &(m)->v[_i], 1); ++_i) \
      if (bstest((m)->mb.bs, _i))
#define imap_copy(dst,src) do {                                                       \
   size_t N = (src)->mb.N;                                                            \
   if (!N) break;                                                                     \
   (dst)->mb.n = (src)->mb.n;                                                         \
   imap_init((dst), N);                                                               \
   memcpy((dst)->mb.k, (src)->mb.k,                                                   \
         N*(sizeof*(src)->mb.k + sizeof*(src)->v) + BSSIZE(N)*sizeof(struct bitset)); \
} while (0)


/** map of non-null ptr -> T **/
#define pmap_of(T) struct { T *v; int tmp; struct pmapbase mb; }
struct pmapbase { void **k; uint n, N; };

void pmap_init_(struct pmapbase *, void **v, uint vsiz, uint N);
int pmap_get_(struct pmapbase *, const void *k);
int pmap_set_(struct pmapbase *, void **v, uint vsiz, const void *k);
#define pmap_free(m) (free((m)->mb.k), memset((m), 0, sizeof *(m)))
#define pmap_init(m, N) (pmap_free(m), pmap_init_(&(m)->mb, (void **)&(m)->v, sizeof*(m)->v, (N))
#define pmap_get(m, k) (((m)->tmp = pmap_get_(&(m)->mb, k)) < 0 ? NULL : &(m)->v[(m)->tmp])
#define pmap_set(m, k, x) ((m)->tmp = pmap_set_(&(m)->mb, (void **)&(m)->v, sizeof*(m)->v, k), \
                           (m)->v[(m)->tmp] = (x))
#define pmap_each(m,kx,pvx) \
   for (int _i = 0; _i < (m)->mb.N && ((kx) = (m)->mb.k[_i], (pvx) = &(m)->v[_i], 1); ++_i) \
      if (kx)

/** bitset **/
struct bitset { uvlong u; };
enum { BSNBIT = 8 * sizeof(uvlong) };
#define BSSIZE(nbit) ((nbit) / BSNBIT + ((nbit) % BSNBIT != 0))

static inline bool
bstest(const struct bitset *bs, uint i)
{
   return bs[i / BSNBIT].u >> i % BSNBIT & 1;
}

static inline void
bsset(struct bitset *bs, uint i)
{
   bs[i / BSNBIT].u |= 1ull << i % BSNBIT;
}

static inline void
bsclr(struct bitset *bs, uint i)
{
   bs[i / BSNBIT].u &= ~(1ull << i % BSNBIT);
}

static inline void
bszero(struct bitset bs[/*siz*/], uint siz)
{
   memset(bs, 0, siz * sizeof *bs);
}

static inline void
bscopy(struct bitset dst[/*siz*/], const struct bitset src[/*siz*/], uint siz)
{
   while (siz--) dst++->u = src++->u;
}

static inline void
bsunion(struct bitset dst[/*siz*/], const struct bitset src[/*siz*/], uint siz)
{
   while (siz--) dst++->u |= src++->u;
}

static inline uint
bscount(struct bitset bs[/*siz*/], uint siz)
{
   uint n = 0;
   while (siz--) n += popcnt(bs++->u);
   return n;
}

static inline bool
bsiter(uint *i, struct bitset bs[/*siz*/], uint siz)
{
   for (; *i < siz*BSNBIT; ++*i)
      if (bstest(bs, *i)) return 1;
   return 0;
}
#define bs_each(T, var, bs, siz) for (T (var) = 0; bsiter(&(var), (bs), (siz)); ++(var))

/********/
/** IO **/
/********/

struct wbuf {
   union {
      struct {
         char *buf;
         uint cap;
         uint len;
         int fd;
      };
      void *fp;
   };
   bool err;
   bool isfp;
};

/* read-only file mapping that is at least 1 page larger than the real file
 * so it's legal to read a few bytes beyond it to avoid some bounds checks */
struct memfile {
   const uchar *p;
   uint n;
   bool statik;
};

struct embedfile {
   const char *name;
   const char *s;
   size_t len;
};

#define MEMBUF(buf_, cap_) { .buf = (buf_), .cap = (cap_), .fd = -1 }
#define FDBUF(buf_, cap_, fd_)  { .buf = (buf_), .cap = (cap_), .fd = (fd_) }
extern struct wbuf bstdout, bstderr;
void ioinit(void);
void iowrite(struct wbuf *, const void *src, int n);
void ioputc(struct wbuf *, uchar);
void ioflush(struct wbuf *);
int vbfmt(struct wbuf *, const char *, va_list ap);
int bfmt(struct wbuf *, const char *, ...);
#define pfmt(...) bfmt(&bstdout, __VA_ARGS__)
#define efmt(...) bfmt(&bstderr, __VA_ARGS__)
struct memfile mapopen(const char **err, const char *path);
void mapclose(struct memfile *);
void *mapzeros(uint);
int munmap(void *, size_t);
int getpredeffile(struct memfile **, const char *name);
int openfile(const char **err, struct memfile **, const char *path);
const char *getfilename(int id);
struct memfile *getfile(int id);
void addfileline(int id, uint off);
void getfilepos(int *line, int *col, int id, uint off);
bool isoncefile(int id, const char **guard);
void markfileonce(int id, const char *guard);
void markfileseen(int id);
bool isfileseen(int id);
void closefile(int id);
void fatal(const struct span *, const char *, ...);
void error(const struct span *, const char *, ...);
void warn(const struct span *, const char *, ...);
void note(const struct span *, const char *, ...);
ushort *utf8to16(uint *ulen, struct arena **, const uchar *s, size_t len);
uint *utf8to32(uint *ulen, struct arena **, const uchar *s, size_t len);
int utf8enc(char out[4], uint cp);

#endif /* COMMON_H_ */

/* vim:set ts=3 sw=3 expandtab: */
