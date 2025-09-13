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
struct bytes { uchar *p; uint n; };

struct span {
   struct span0 {
      uint off;
      uint len : 24,
           file :  8;
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
ilog2(uint x) { /* assumes x is a power of 2 */
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
};
extern struct option ccopt;

/*************************/
/** TYPE REPRESENTATION **/
/*************************/

enum qualifier {
   QCONST    = 1<<0,
   QVOLATILE = 1<<1,
   QNORETURN = 1<<2, /* functions */
   QINLINE   = 1<<3, /* functions */
};

enum typetag { /* ordering is important here! */
   TYXXX,
   TYENUM,
   TYBOOL, TYCHAR, TYSCHAR, TYUCHAR, TYSHORT, TYUSHORT, TYINT, TYUINT, TYLONG, TYULONG, TYVLONG, TYUVLONG,
   TYFLOAT, TYDOUBLE, TYLDOUBLE,
   TYVOID,
   TYVALIST,
   TYPTR, TYARRAY, TYFUNC,
   TYSTRUCT, TYUNION,
   NTYPETAG,
   TYSIGNEDSET_ = 1<<TYSCHAR | 1<<TYSHORT | 1<<TYINT | 1<<TYLONG | 1<<TYVLONG,
   TYUNSIGNEDSET_ = 1<<TYUCHAR | 1<<TYUSHORT | 1<<TYUINT | 1<<TYULONG | 1<<TYUVLONG,
   TYSCALARSET_ = ((1u << (TYLDOUBLE - TYENUM + 1)) - 1) << TYENUM | 1<<TYPTR
};

enum typeflagmask {
   TFCHLDQUAL = 3,
   TFCHLDPRIM = 1<<2,
   TFCHLDISDAT = 1<<3,
};

union type {
   struct {
      uchar t; /* type tag */
      union {
         uchar flag;
         uchar backing; /* type tag for enum backing int */
      };
      union {
         struct {
            uchar child; /* prim type tag */
            uchar arrlen; /* small array */
         };
         ushort dat; /* index into typedata */
      };
   };
   uint bits;
};

#define isprimt(t)     in_range((t), TYBOOL, TYVALIST)
#define isintt(t)      in_range((t), TYENUM, TYUVLONG)
#define issignedt(t)   ((TYSIGNEDSET_ | targ_charsigned << TYCHAR) >> (t) & 1)
#define isunsignedt(t) ((TYUNSIGNEDSET_ | !targ_charsigned << TYCHAR) >> (t) & 1)
#define isfltt(t)      in_range((t), TYFLOAT, TYLDOUBLE)
#define isaritht(t)    in_range((t), TYENUM, TYLDOUBLE)
#define isscalart(t)   (TYSCALARSET_ >> (t) & 1)
#define isptrcvtt(t)   in_range((t), TYPTR, TYFUNC)
#define isaggt(t)      in_range((t), TYSTRUCT, TYUNION)
#define isprim(ty)     isprimt((ty).t)
#define isint(ty)      isintt((ty).t)
#define issigned(ty)   issignedt((ty).t)
#define isunsigned(ty) isunsignedt((ty).t)
#define isflt(ty)      isfltt((ty).t)
#define isarith(ty)    isaritht((ty).t)
#define isscalar(ty)   isscalart((ty).t)
#define isptrcvt(ty)   isptrcvtt((ty).t)
#define isagg(ty)      isaggt((ty).t)
#define mktype(...)   ((union type) {{ __VA_ARGS__ }})

struct enumvar {
   const char *name;
   union { vlong i; uvlong u; };
};

struct fielddata {
   union type t;
   ushort off;
   uchar bitsiz,
         bitoff : 6,
         qual : 2;
};
struct namedfield {
   const char *name;
   struct fielddata f;
};

struct typedata {
   uchar t;
   ushort id;
   union {
      union type child;
      struct { /* functions */
         const uchar *quals; /* packed N x 2bit array (NULL if no param has quals) */
         const union type *param;
      };
      struct { /* aggregates */
        /* struct fieldmap {
            union {
               struct field *fs;
               int *is;
            };
            const char **k;
            uint ;
         } *fmap; */
         struct namedfield *fld;
      };
      struct { /* enum */
         uchar backing;
         struct enumvar *var;
      };
   };
   union {
      uint arrlen; /* array */
      struct {
         short nmemb; /* functions, aggregates, enums */
         uchar align;
         union {
            struct { /* function */
               bool kandr : 1, variadic : 1;
            };
            struct { /* aggregate */
               bool anyconst : 1, flexi : 1;
            };
         };
      };
   };
   union {
      uint siz; /* aggregate & array */
      union type ret; /* function */
   };
};

extern struct typedata typedata[];
extern const char *ttypenames[/*id*/];

#define tdqualsiz(nmemb) ((nmemb)/4 + ((nmemb)%4 != 0))
static inline int
tdgetqual(const uchar *pqual, int idx)
{
   return pqual ? pqual[idx/4] >> 2*(idx%4) & 3 : 0;
}
static inline void
tdsetqual(uchar *pqual, int idx, int qual)
{
   assert(pqual);
   pqual[idx/4] &= ~(3 << (2*(idx%4)));
   pqual[idx/4] |= (qual&3) << (2*(idx%4));
}

bool isincomplete(union type);
uint typesize(union type);
uint typealign(union type);
union type mkptrtype(union type, int qual);
union type mkarrtype(union type t, int qual, uint n);
union type mkfntype(union type ret, uint n, const union type *, const uchar *qual, bool kandr, bool variadic);
union type mktagtype(const char *name, struct typedata *td);
bool getfield(struct fielddata *res, union type, const char *);
union type completetype(const char *name, int id, struct typedata *td);
union type typedecay(union type);
bool assigncompat(union type dst, union type src);
enum typetag intpromote(enum typetag);
union type cvtarith(union type a, union type b);
static inline union type
typechild(union type t)
{
   if (t.t == TYENUM) return mktype(t.backing);
   if (t.flag & TFCHLDPRIM) return mktype(t.child);
   if (t.flag & TFCHLDISDAT) {
      union type chld = mktype(typedata[t.dat].t, .dat = t.dat);
      if (chld.t == TYENUM) chld.backing = typedata[t.dat].backing;
      return chld;
   }
   return typedata[t.dat].child;
}
static inline uint
typearrlen(union type t)
{
   return (t.flag & TFCHLDPRIM) ? t.arrlen : typedata[t.dat].arrlen;
}

/**********/
/* TARGET */
/**********/

extern uchar targ_primsizes[];
extern uchar targ_primalign[];
extern enum typetag targ_sizetype, targ_ptrdifftype;
extern bool targ_charsigned, targ_bigendian, targ_64bit;
extern const struct mctarg *mctarg;
void targ_init(const char *);

/*********/
/** MEM **/
/*********/

struct arena {
   uint cap : 31,
        dyn : 1;
   uint n;
   struct arena *prev;
   uchar mem[];
};

#define vec_of(T) struct { T *p; int _cap; uint n; }

/* libc *alloc wrappers */
extern void *xcalloc(size_t n, const char *);
extern void *xrealloc(void *, size_t n, const char *);
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

struct arena *newarena(uint chunksiz);
void *alloc(struct arena **, uint siz, uint align);
void *allocz(struct arena **, uint siz, uint align);
static inline void *
alloccopy(struct arena **arena, const void *src, uint siz, uint align)
{
   return memcpy(alloc(arena, siz, align), src, siz);
}
void freearena(struct arena *);

void vinit_(void **p, int *pcap, void *inlbuf, int cap, uint siz);
void vpush_(void **p, int *pcap, uint *pn, uint siz);
void *vpushn_(void **p, int *pcap, uint *pn, uint siz, const void *dat, uint ndat);
void vresize_(void **p, int *pcap, uint *pn, uint siz, uint N);
extern void free(void *);
#define VINIT(inlbuf, Cap) { (inlbuf), (Cap) }
#define vfree(v) ((v)->_cap < 0 ? free((v)->p) : (void)0, memset((v), 0, sizeof*(v)))
#define vinit(v, inlbuf, Cap) (vfree(v), vinit_((void **)&(v)->p, &(v)->_cap, inlbuf, (Cap), sizeof *(v)->p))
#define vpush(v, x) (vpush_((void **)&(v)->p, &(v)->_cap, &(v)->n, sizeof *(v)->p), \
                     (v)->p[(v)->n++] = (x))
#define vpushn(v, xs, N) vpushn_((void **)&(v)->p, &(v)->_cap, &(v)->n, sizeof *(v)->p, xs, N)
#define vresize(v, N) vresize_((void **)&(v)->p, &(v)->_cap, &(v)->n, sizeof *(v)->p, N)

struct bitset { uvlong u; };
enum { BSNBIT = 8 * sizeof(uvlong) };
#define BSSIZE(nbit) ((nbit) / BSNBIT + ((nbit) % BSNBIT != 0))

struct imapbase { short *k; struct bitset *bs; uint n, N; };
/* map of short -> T */
#define imap_of(T) struct { T *v; int tmp; struct imapbase mb; }
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


struct pmapbase { void **k; uint n, N; };
/* map of non-null ptr -> T */
#define pmap_of(T) struct { T *v; int tmp; struct pmapbase mb; }
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

static inline bool
bsiter(uint *i, struct bitset bs[/*siz*/], uint siz)
{
   for (; *i < siz*BSNBIT; ++*i)
      if (bstest(bs, *i)) return 1;
   return 0;
}
#define bs_each(T, var, bs, siz) for (T (var) = 0; bsiter(&(var), (bs), (siz)); ++(var))

typedef uvlong regset;
#define rsset(S, r) ((S) | 1ull << (r))
#define rsclr(S, r) ((S) & ~(1ull << (r)))
#define rstest(S, r) ((S) >> (r) & 1)
#define rsminus(A, B) ((A) & ~(B))
#define rsand(A, B) ((A) & (B))
#define rsunion(A, B) ((A) | (B))
static inline bool
rsiter(int *i, uvlong rs)
{
   uvlong mask = -(1ull << *i);
   if ((rs & mask) == 0) return 0;
   *i = lowestsetbit(rs & mask);
   return 1;
}

/********/
/** IO **/
/********/

struct wbuf {
   char *buf;
   const uint cap;
   uint len;
   const int fd;
   bool err;
};

/* read-only file mapping that is at least 1 page larger than the real file
 * so it's legal to read a few bytes beyond it to avoid some bounds checks */
struct memfile {
   const uchar *p;
   uint n;
};

#define MEMBUF(buf, cap) { (buf), (cap), .fd = -1 }
#define FDBUF(buf, cap, fd_)  { (buf), (cap), .fd = (fd_) }
extern struct wbuf bstdout, bstderr;
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
int openfile(const char **err, struct memfile **, const char *path);
const char *getfilename(int id);
struct memfile *getfile(int id);
void addfileline(int id, uint off);
void getfilepos(int *line, int *col, int id, uint off);
void closefile(int id);
void fatal(const struct span *, const char *, ...);
void error(const struct span *, const char *, ...);
void warn(const struct span *, const char *, ...);
void note(const struct span *, const char *, ...);

#endif /* COMMON_H_ */

/* vim:set ts=3 sw=3 expandtab: */
