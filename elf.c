#include "elf.h"
#include "common.h"
#include "obj.h"
#include "ir.h"
#include <unistd.h>
#include <stdlib.h> /* qsort */

static struct elfhdr hdr;
static vec_of(uchar) strs;
static vec_of(struct elfsym) symtab;
struct reloc {
   uchar section;
   ushort kind;
   uint sym;
   uint off;
   vlong addend;
};
uchar dataalign = 1, rodataalign = 1, bssalign = 1;
uint nbss;
static vec_of(uchar) data, rodata;
static uint ntextrel, nrodatarel, ndatarel;
static vec_of(struct reloc) relocs;

void
elfinit(void)
{
   memcpy(hdr.i_mag, ELFMAG, 4);
   hdr.i_class = ELFCLASS32 + targ_64bit;
   hdr.i_data = ELFDATA2LSB + targ_bigendian;
   hdr.i_version = ELFVERSION;
   hdr.i_osabi = ELFOSABI_SYSV;
   hdr.i_abiversion = 0;
   hdr.type = ET_REL;
   switch (mctarg->isa) {
   case ISamd64: hdr.machine = EM_X86_64; break;
   }
   hdr.version = ELFVERSION;
   hdr.ehsize = targ_64bit ? 64 : 52;
   hdr.shentsize = targ_64bit ? 64 : 40;
   vpush(&symtab, ((struct elfsym) { 0 }));
   vpush(&symtab, ((struct elfsym) { 0, ELF_S_INFO(0, STT_FILE) }));
}

uint
str2idx(const char *s)
{
   static pmap_of(uint) ht;
   uint *p, i;

   if ((p = pmap_get(&ht, s))) return *p;
   if (!strs.n) vpush(&strs, 0);
   i = strs.n;
   vpushn(&strs, s, strlen(s)+1);
   pmap_set(&ht, s, i);
   return i;
}

static struct elfsym *
findsym(uint name)
{
   for (int i = 0; i < symtab.n; ++i)
      if (symtab.p[i].name == name)
         return &symtab.p[i];
   return NULL;
}

enum {
   TEXT_SHNDX   = 1,
   RODATA_SHNDX = 2,
   DATA_SHNDX   = 3,
   BSS_SHNDX    = 4,
};

void
elfaddsym(const char *nam, int info, enum section sect, uvlong value, uvlong size)
{
   uint str = str2idx(nam);
   struct elfsym *sym = findsym(str), sym0 = {0};

   if (!sym) {
      sym = &sym0;
      sym->name = str;
   }
   sym->info = info;
   sym->other = 0;
   switch (sect) {
   case Snone:   sym->shndx = SHN_UND; break;
   case Stext:   sym->shndx = TEXT_SHNDX; break;
   case Srodata: sym->shndx = RODATA_SHNDX; break;
   case Sdata:   sym->shndx = DATA_SHNDX; break;
   case Sbss:    sym->shndx = BSS_SHNDX; break;
   }
   sym->value = value;
   sym->size = size;
   if (sym == &sym0) vpush(&symtab, sym0);
}

static const ushort relktab[][NRELOCKIND] = {
   [ISamd64] = {
      [REL_ABS]     = 5, /* R_X86_64_COPY */
      [REL_PCREL32] = 2, /* R_X86_64_PC32 */
   }
};

void
elfreloc(const char *sym, enum relockind kind, enum section section, uint off, vlong addend)
{
   uint snam = str2idx(sym);
   switch (section) {
   default: assert(0);
   case Stext:   ++ntextrel; break;
   case Srodata: ++nrodatarel; break;
   case Sdata:   ++ndatarel; break;
   }
   assert(kind < NRELOCKIND);
   vpush(&relocs, ((struct reloc) { section, relktab[mctarg->isa][kind], snam, off, addend}));
}

void
elfputdat(const struct irdat *dat)
{
   static const char zero[8];
   enum section s;
   uint off;
   uint ndat = dat->siz < 8 ? dat->siz : dat->dat.n;
   uint nzr = dat->siz - ndat; 
   const uchar *d = dat->siz < 8 ? dat->sdat : dat->dat.p;

   assert(dat->siz);
   if (!dat->syms && (dat->siz >= 8 ? !d : !memcmp(d, zero, dat->siz))) {
      /* all zeroes */
      s = Sbss;
   } else {
      s = dat->mut ? Sdata : Srodata;
   }

   switch (s) {
   default: assert(0);
   case Srodata:
      if (dat->align > rodataalign) rodataalign = dat->align;
      while (rodata.n & (dat->align - 1)) vpush(&rodata, 0);
      off = rodata.n;
      vpushn(&rodata, d, ndat);
      while (nzr--) vpush(&rodata, 0);
      break;
   case Sdata:
      if (dat->align > dataalign) dataalign = dat->align;
      while (data.n & (dat->align - 1)) vpush(&data, 0);
      off = data.n;
      vpushn(&data, d, ndat);
      while (nzr--) vpush(&data, 0);
      break;
   case Sbss:
      if (dat->align > bssalign) bssalign = dat->align;
      off = alignup(nbss, dat->align);
      nbss = off + dat->siz;
      break;
   }
   elfaddsym(dat->name, ELF_S_INFO(dat->globl, STT_OBJECT), s, off, dat->siz);
}

static void
put16le(struct wbuf *out, ushort x)
{
   uchar b[2] = { x, x >> 8 };
   iowrite(out, b, 2);
}

static void
put16be(struct wbuf *out, ushort x)
{
   uchar b[2] = { x >> 8, x };
   iowrite(out, b, 2);
}

static void
put32le(struct wbuf *out, uint x)
{
   uchar b[4] = { x, x >> 8, x >> 16, x >> 24 };
   iowrite(out, b, 4);
}

static void
put32be(struct wbuf *out, uint x)
{
   uchar b[4] = { x >> 24, x >> 16, x >> 8, x };
   iowrite(out, b, 4);
}

static void
putword64le(struct wbuf *out, uvlong x)
{
   uchar b[8] = { x, x >> 8, x >> 16, x >> 24, x >> 32, x >> 40, x >> 48, x >> 56 };
   iowrite(out, b, 8);
}

static void
putword64be(struct wbuf *out, uvlong x)
{
   uchar b[8] = { x >> 56, x >> 48, x >> 40, x >> 32, x >> 24, x >> 16, x >> 8, x };
   iowrite(out, b, 8);
}

static void
putword32le(struct wbuf *out, uvlong x) { put32le(out, x); }
static void
putword32be(struct wbuf *out, uvlong x) { put32be(out, x); }

static void (*putword)(struct wbuf *, uvlong);
static void (*put16)(struct wbuf *, ushort);
static void (*put32)(struct wbuf *, uint);

static void
elfputshdr(struct wbuf *out, const struct elfshdr *shdr)
{
   put32(out, shdr->name);
   put32(out, shdr->type);
   putword(out, shdr->flags);
   putword(out, shdr->addr);
   putword(out, shdr->offset);
   putword(out, shdr->size);
   put32(out, shdr->link);
   put32(out, shdr->info);
   putword(out, shdr->addralign);
   putword(out, shdr->entsize);
}

static void
elfputsym(struct wbuf *out, const struct elfsym *sym)
{
   put32(out, sym->name);
   ioputc(out, sym->info);
   ioputc(out, sym->other);
   put16(out, sym->shndx);
   putword(out, sym->value);
   putword(out, sym->size);
}

static void
elfputrel(struct wbuf *out, const struct elfrel *rel)
{
   putword(out, rel->offset);
   putword(out, rel->info);
}

static void
elfputrela(struct wbuf *out, const struct elfrela *rel)
{
   putword(out, rel->offset);
   putword(out, rel->info);
   putword(out, rel->addend);
}

/* ensure .symtab entries are ordered like this:
 * (0. zero entry: NOTYPE LOCAL UND)
 * (1. file: FILE LOCAL UND "...")
 * 2. locals
 * 3. defined globals
 * 4. undefined globals
 */
static int
symcmp(const void *L, const void *R)
{
   const struct elfsym *l = L, *r = R;
   int tmp, lbind = l->info >> 4, rbind = r->info >> 4;
   if ((tmp = lbind - rbind)) return tmp; /* locals prio */
   if ((tmp = r->shndx - l->shndx)) return tmp; /* section prio (real sections > SHN_UND) */
   return l->name - r->name;
}

static void
wordalign(struct wbuf *out, int align)
{
   size_t off = out->len + lseek(out->fd, 0, SEEK_CUR);
   while (off++ & (align - 1)) ioputc(out, 0);
}

void
elffini(struct wbuf *out)
{
   enum { shnam_text = 1, shnam_rodata = 7, shnam_data = 15, shnam_bss = 21, shnam_shstrtab = 26,
          shnam_strtab = 36, shnam_symtab = 44, shnam_reltext = 52, shnam_relrodata = 62, shnam_reldata = 74 };
   static const char shstrs[] = "\0.text\0.rodata\0.data\0.bss\0.shstrtab\0.strtab\0.symtab\0"
                                ".rel.text\0.rel.rodata\0.rel.data";
   int align = targ_64bit ? 8 : 4;

   symtab.p[1].name = str2idx(getfilename(0));
   qsort(symtab.p+2, symtab.n-2, sizeof *symtab.p, symcmp);
   /* fixup relocs */
   for (int i = 0; i < relocs.n; ++i) {
      struct reloc *rel = &relocs.p[i];
      struct elfsym *sym;
      sym = findsym(rel->sym);
      if (sym) rel->sym = sym - symtab.p;
      else {
         sym = &(struct elfsym) { rel->sym, ELF_S_INFO(STB_GLOBAL, STT_NOTYPE), 0, SHN_UND };
         rel->sym = symtab.n;
         vpush(&symtab, *sym);
      }
   }
   size_t codesize = alignup(objout.code - objout.textbegin, align),
          rodataoff = hdr.ehsize + codesize,
          rodatasize = rodata.n,
          dataoff = rodataoff + rodatasize,
          datasize = data.n,
          bsssize = nbss,
          shstrsoff = dataoff + datasize,
          shstrssize = sizeof(shstrs),
          strsoff = shstrsoff + shstrssize,
          strssize = strs.n,
          symtaboff = alignup(strsoff + strssize, align),
          symtabsize = symtab.n * (targ_64bit ? 24 : 16),
          reltextoff = symtaboff + symtabsize,
          reltextsize = ntextrel * (targ_64bit ? 24 : 12),
          relrodataoff = reltextoff + reltextsize,
          relrodatasize = nrodatarel * (targ_64bit ? 24 : 12),
          reldataoff = relrodataoff + relrodatasize,
          reldatasize = ndatarel * (targ_64bit ? 24 : 12);
   int nlocal = 0;

   put16 = !targ_bigendian ? put16le : put16be;
   put32 = !targ_bigendian ? put32le : put32be;
   if (!targ_bigendian) putword = !targ_64bit ? putword32le : putword64le;
   else                 putword = !targ_64bit ? putword32be : putword64be;

   hdr.shoff = alignup(reldataoff + reldatasize, align);
   hdr.shnum = 11;
   hdr.shstrndx = 5;

   /* elf header */
   iowrite(out, &hdr, offsetof(struct elfhdr, type));
   put16(out, hdr.type);
   put16(out, hdr.machine);
   put32(out, hdr.version);
   putword(out, hdr.entry);
   putword(out, hdr.phoff);
   putword(out, hdr.shoff);
   put32(out, hdr.flags);
   put16(out, hdr.ehsize);
   put16(out, hdr.phentsize);
   put16(out, hdr.phnum);
   put16(out, hdr.shentsize);
   put16(out, hdr.shnum);
   put16(out, hdr.shstrndx);

   /* .text progbits */
   iowrite(out, objout.textbegin, codesize);

   /* .rodata progbits */
   iowrite(out, rodata.p, rodata.n);

   /* .data progbits */
   iowrite(out, data.p, data.n);

   /* section names */
   iowrite(out, shstrs, sizeof shstrs);

   /* strings */
   iowrite(out, strs.p, strs.n);

   /* symtab */
   wordalign(out, align);
   for (int i = 0; i < symtab.n; ++i) {
      struct elfsym *sym = &symtab.p[i];
      if (sym->info >> 4 == STB_LOCAL) ++nlocal;
      elfputsym(out, sym);
   }

   /* rel.* */
   assert(relocs.n == ntextrel + nrodatarel + ndatarel);
   for (enum section s = Stext; s <= Sbss; ++s) {
      for (int i = 0; i < relocs.n; ++i) {
         struct reloc *rel = &relocs.p[i];
         if (rel->section != s) continue;
         elfputrela(out, &(struct elfrela) {rel->off, ELF_R_INFO(rel->sym, rel->kind), rel->addend});
      }
   }

   /** Section Headers **/
   wordalign(out, align);
   /* §0 null section */
   elfputshdr(out, &(struct elfshdr){0});
   /* §1 .text */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_text, .type = SHT_PROGBITS,
         .flags = SHF_ALLOC | SHF_EXECINSTR,
         .offset = hdr.ehsize, .size = codesize,
         .addralign = align,});
   /* §2 .rodata */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_rodata, .type = SHT_PROGBITS,
         .flags = SHF_ALLOC,
         .offset = rodataoff, .size = rodatasize,
         .addralign = rodataalign,});
   /* §3 .data */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_data, .type = SHT_PROGBITS,
         .flags = SHF_ALLOC | SHF_WRITE,
         .offset = dataoff, .size = datasize,
         .addralign = dataalign,});
   /* §4 .bss */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_bss, .type = SHT_NOBITS,
         .size = bsssize,
         .flags = SHF_ALLOC | SHF_WRITE,
         .addralign = bssalign,});
   /* §5 .shstrtab */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_shstrtab, .type = SHT_STRTAB,
         .offset = shstrsoff, .size = shstrssize,
         .flags = SHF_STRINGS });
   /* §6 .strtab */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_strtab, .type = SHT_STRTAB,
         .offset = strsoff, .size = strssize,
         .flags = SHF_STRINGS });
   /* §7 .symtab */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_symtab, .type = SHT_SYMTAB,
         .offset = symtaboff, .size = symtabsize,
         .flags = SHF_STRINGS,
         .link = 6, /* .strtab */
         .info = nlocal,
         .entsize = targ_64bit ? 24 : 16 });
   /* §8 .rel.text */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_reltext, .type = SHT_RELA,
         .offset = reltextoff, .size = reltextsize,
         .link = 7, /* .symtab */
         .entsize = targ_64bit ? 24 : 12,
         .info = TEXT_SHNDX });
   /* §9 .rel.rodata */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_relrodata, .type = SHT_RELA,
         .offset = relrodataoff, .size = relrodatasize,
         .link = 7, /* .symtab */
         .entsize = targ_64bit ? 24 : 12,
         .info = RODATA_SHNDX });
   /* §10 .rel.data */
   elfputshdr(out, &(struct elfshdr){
         .name = shnam_reldata, .type = SHT_RELA,
         .offset = reldataoff, .size = reldatasize,
         .link = 7, /* .symtab */
         .entsize = targ_64bit ? 24 : 12,
         .info = DATA_SHNDX });
}

/* vim:set ts=3 sw=3 expandtab: */
