#include "elf.h"
#include "common.h"
#include "obj.h"
#include "ir.h"
#include "endian.h"
#include <unistd.h>
#include <stdlib.h> /* qsort */

static union {
   ELF_HDRIDENT;
   struct elf32hdr h32;
   struct elf64hdr h64;
} hdr;
static vec_of(uchar) strs;
struct sym {
   uint   name;
   uchar  bind : 4,
          type : 4;
   ushort shndx;
   uvlong value,
          size;
};
static vec_of(struct sym) symtab;
static uint ntextrel, nrodatarel, ndatarel;
struct reloc {
   uchar section;
   ushort kind;
   uint sym;
   uint off;
   vlong addend;
};
static vec_of(struct reloc) relocs;

#define O objout

void
elfinit(void)
{
   memcpy(hdr.i_mag, ELFMAG, 4);
   hdr.i_class = ELFCLASS32 + targ_64bit;
   hdr.i_data = ELFDATA2LSB + targ_bigendian;
   hdr.i_version = ELFVERSION;
   hdr.i_osabi = ELFOSABI_SYSV;
   hdr.i_abiversion = 0;
   hdr.h32.type = ET_REL;
   switch (mctarg->isa) {
   case ISamd64: hdr.h32.machine = EM_X86_64; break;
   }
   hdr.h32.version = ELFVERSION;
   if (targ_64bit) {
      hdr.h64.ehsize = sizeof(struct elf64hdr);
      hdr.h64.shentsize = sizeof(struct elf64shdr);
   } else {
      hdr.h32.ehsize = sizeof(struct elf32hdr);
      hdr.h32.shentsize = sizeof(struct elf32shdr);
   }
   vpush(&symtab, ((struct sym) { 0 }));
   vpush(&symtab, ((struct sym) { .type = STT_FILE, .shndx = SHN_ABS }));
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

static struct sym *
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
   struct sym *sym = findsym(str), sym0 = {0};

   if (!sym) {
      sym = &sym0;
      sym->name = str;
   }
   sym->bind = info >> 4;
   sym->type = info & 0xF;
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
      [REL_ABS64]   = 1, /* R_X86_64_64 */
      [REL_ABS32]   = 10, /* R_X86_64_32 */
      [REL_ABS32S]  = 11, /* R_X86_64_32S */
      [REL_PCREL32] = 2, /* R_X86_64_PC32 */
      [REL_PLT32]   = 4, /* R_X86_64_PLT32 */
      [REL_GOTPCRELX]     = 41, /* R_X86_64_GOTPCRELX */
      [REL_GOTPCRELX_REX] = 42, /* R_X86_64_REX_GOTPCRELX */
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
   vpush(&relocs, ((struct reloc) { section, relktab[mctarg->isa][kind], snam, off, addend }));
}

static void
elf64puthdr(struct wbuf *out, struct elf64hdr *hdr)
{
   if (!hostntarg_sameendian()) {
      hdr->type = bswap16(hdr->type);
      hdr->machine = bswap16(hdr->machine);
      hdr->version = bswap32(hdr->version);
      hdr->entry = bswap64(hdr->entry);
      hdr->phoff = bswap64(hdr->phoff);
      hdr->shoff = bswap64(hdr->shoff);
      hdr->flags = bswap32(hdr->flags);
      hdr->ehsize = bswap16(hdr->ehsize);
      hdr->phentsize = bswap16(hdr->phentsize);
      hdr->phnum = bswap16(hdr->phnum);
      hdr->shentsize = bswap16(hdr->shentsize);
      hdr->shnum = bswap16(hdr->shnum);
      hdr->shstrndx = bswap16(hdr->shstrndx);
   }
   iowrite(out, hdr, sizeof *hdr);
}

static void
elf32puthdr(struct wbuf *out, struct elf32hdr *hdr)
{
   if (!hostntarg_sameendian()) {
      hdr->type = bswap16(hdr->type);
      hdr->machine = bswap16(hdr->machine);
      hdr->version = bswap32(hdr->version);
      hdr->entry = bswap32(hdr->entry);
      hdr->phoff = bswap32(hdr->phoff);
      hdr->shoff = bswap32(hdr->shoff);
      hdr->flags = bswap32(hdr->flags);
      hdr->ehsize = bswap16(hdr->ehsize);
      hdr->phentsize = bswap16(hdr->phentsize);
      hdr->phnum = bswap16(hdr->phnum);
      hdr->shentsize = bswap16(hdr->shentsize);
      hdr->shnum = bswap16(hdr->shnum);
      hdr->shstrndx = bswap16(hdr->shstrndx);
   }
   iowrite(out, hdr, sizeof *hdr);
}

static void
elf64putshdr(struct wbuf *out, struct elf64shdr *shdr)
{
   if (!hostntarg_sameendian()) {
      shdr->name = bswap32(shdr->name);
      shdr->type = bswap32(shdr->type);
      shdr->flags = bswap64(shdr->flags);
      shdr->addr = bswap64(shdr->addr);
      shdr->offset = bswap64(shdr->offset);
      shdr->size = bswap64(shdr->size);
      shdr->link = bswap32(shdr->link);
      shdr->info = bswap32(shdr->info);
      shdr->addralign = bswap64(shdr->addralign);
      shdr->entsize = bswap64(shdr->entsize);
   }
   iowrite(out, shdr, sizeof *shdr);
}

static void
elf32putshdr(struct wbuf *out, struct elf32shdr *shdr)
{
   if (!hostntarg_sameendian()) {
      shdr->name = bswap32(shdr->name);
      shdr->type = bswap32(shdr->type);
      shdr->flags = bswap32(shdr->flags);
      shdr->addr = bswap32(shdr->addr);
      shdr->offset = bswap32(shdr->offset);
      shdr->size = bswap32(shdr->size);
      shdr->link = bswap32(shdr->link);
      shdr->info = bswap32(shdr->info);
      shdr->addralign = bswap32(shdr->addralign);
      shdr->entsize = bswap32(shdr->entsize);
   }
   iowrite(out, shdr, sizeof *shdr);
}

static void
elf64putsym(struct wbuf *out, struct elf64sym *sym)
{
   if (!hostntarg_sameendian()) {
      sym->name = bswap32(sym->name);
      sym->shndx = bswap16(sym->shndx);
      sym->value = bswap64(sym->value);
      sym->size = bswap64(sym->size);
   }
   iowrite(out, sym, sizeof *sym);
}

static void
elf32putsym(struct wbuf *out, struct elf32sym *sym)
{
   if (!hostntarg_sameendian()) {
      sym->name = bswap32(sym->name);
      sym->value = bswap32(sym->value);
      sym->size = bswap32(sym->size);
      sym->shndx = bswap16(sym->shndx);
   }
   iowrite(out, sym, sizeof *sym);
}

static void
putsym(struct wbuf *out, const struct sym *sym)
{
   if (targ_64bit) {
      elf64putsym(out, &(struct elf64sym) {
            sym->name, .info = ELF_S_INFO(sym->bind, sym->type),
            .shndx = sym->shndx, .value = sym->value, .size = sym->size });
   } else {
      elf32putsym(out, &(struct elf32sym) {
            sym->name, .info = ELF_S_INFO(sym->bind, sym->type),
            .shndx = sym->shndx, .value = sym->value, .size = sym->size });
   }
}

static void
elf64putrel(struct wbuf *out, struct elf64rel *rel)
{
   if (!hostntarg_sameendian()) {
      rel->offset = bswap64(rel->offset);
      rel->info = bswap64(rel->info);
   }
   iowrite(out, rel, sizeof *rel);
}

static void
elf32putrel(struct wbuf *out, struct elf32rel *rel)
{
   if (!hostntarg_sameendian()) {
      rel->offset = bswap32(rel->offset);
      rel->info = bswap32(rel->info);
   }
   iowrite(out, rel, sizeof *rel);
}

static void
elf64putrela(struct wbuf *out, struct elf64rela *rel)
{
   if (!hostntarg_sameendian()) {
      rel->offset = bswap64(rel->offset);
      rel->info = bswap64(rel->info);
      rel->addend = bswap64(rel->addend);
   }
   iowrite(out, rel, sizeof *rel);
}

static void
elf32putrela(struct wbuf *out, struct elf32rela *rel)
{
   if (!hostntarg_sameendian()) {
      rel->offset = bswap32(rel->offset);
      rel->info = bswap32(rel->info);
      rel->addend = bswap32(rel->addend);
   }
   iowrite(out, rel, sizeof *rel);
}

static void
putreloc(struct wbuf *out, const struct reloc *rel, bool userela)
{
   if (userela) {
      if (targ_64bit) {
         elf64putrela(out, &(struct elf64rela) {
               rel->off, ELF64_R_INFO(rel->sym, rel->kind), rel->addend });
      } else {
         elf32putrela(out, &(struct elf32rela) {
               rel->off, ELF32_R_INFO(rel->sym, rel->kind), rel->addend });
      }
   } else {
      if (targ_64bit) {
         elf64putrel(out, &(struct elf64rel) {
               rel->off, ELF64_R_INFO(rel->sym, rel->kind) });
      } else {
         elf32putrel(out, &(struct elf32rel) {
               rel->off, ELF32_R_INFO(rel->sym, rel->kind) });
      }
   }
}

/* ensure .symtab entries are ordered like this:
 * (0. zero entry: NOTYPE LOCAL UND)
 * (1. file: FILE LOCAL ABS "...")
 * 2. locals
 * 3. defined globals
 * 4. undefined globals
 */
static int
symcmp(const void *L, const void *R)
{
   const struct sym *l = L, *r = R;
   int tmp;
   if ((tmp = l->bind - r->bind)) return tmp; /* locals prio */
   if ((tmp = r->shndx - l->shndx)) return tmp; /* section prio (real sections > SHN_UND) */
   return l->name - r->name;
}

static void
wordalign(struct wbuf *out, int align)
{
   size_t off = out->len + lseek(out->fd, 0, SEEK_CUR);
   while (off++ & (align - 1)) ioputc(out, 0);
}

static const bool userelatab[] = { [ISamd64] = 1 };

void
elffini(struct wbuf *out)
{
   enum {
      shnam_text = 1, shnam_rodata = 7, shnam_data = 15, shnam_bss = 21, shnam_shstrtab = 26,
      shnam_strtab = 36, shnam_symtab = 44, shnam_reltext = 52, shnam_relrodata = 63, shnam_reldata = 76
   };
   int align = targ_64bit ? 8 : 4;
   bool userela = userelatab[mctarg->isa];
   char shstrs[] = "\0.text\0.rodata\0.data\0.bss\0.shstrtab\0.strtab\0.symtab\0"
                   ".rela.text\0.rela.rodata\0.rela.data";
   if (!userela) {
      /* .rela -> .rel */
      memcpy(shstrs + shnam_reltext + 4, ".text\0", 6);
      memcpy(shstrs + shnam_relrodata + 4, ".rodata\0", 8);
      memcpy(shstrs + shnam_reldata + 4, ".data\0", 6);
   }

   symtab.p[1].name = str2idx(getfilename(0));
   qsort(symtab.p+2, symtab.n-2, sizeof *symtab.p, symcmp);
   /* fixup relocs */
   for (int i = 0; i < relocs.n; ++i) {
      struct reloc *rel = &relocs.p[i];
      struct sym *sym;
      sym = findsym(rel->sym);
      if (sym) rel->sym = sym - symtab.p;
      else {
         sym = &(struct sym) { rel->sym, .bind = STB_GLOBAL, .type = STT_NOTYPE, .shndx = SHN_UND };
         rel->sym = symtab.n;
         vpush(&symtab, *sym);
      }
      if (!userela) {
         uchar *p;
         switch (rel->section) {
         default: assert(0);
         case Sdata:   p = O.data.p + rel->off; break;
         case Srodata: p = O.rodata.p + rel->off; break;
         case Stext:   p = O.textbegin + rel->off; break;
         }
         if (targ_64bit)
            wr64targ(p, rel->addend);
         else
            wr32targ(p, rel->addend);
      }
   }
   size_t codesize = alignup(O.code - O.textbegin, align),
          rodataoff = (targ_64bit ? sizeof hdr.h64 : sizeof hdr.h32) + codesize,
          rodatasize = O.rodata.n,
          dataoff = rodataoff + rodatasize,
          datasize = O.data.n,
          bsssize = O.nbss,
          shstrsoff = dataoff + datasize,
          shstrssize = sizeof(shstrs),
          strsoff = shstrsoff + shstrssize,
          strssize = strs.n,
          symtaboff = alignup(strsoff + strssize, align),
          symtabsize = symtab.n * (targ_64bit ? 24 : 16),
          reltextoff = symtaboff + symtabsize,
          relxsiz = userela ? (targ_64bit ? 24 : 12) : (targ_64bit ? 16 : 8),
          reltextsize = ntextrel * relxsiz,
          relrodataoff = reltextoff + reltextsize,
          relrodatasize = nrodatarel * relxsiz,
          reldataoff = relrodataoff + relrodatasize,
          reldatasize = ndatarel * relxsiz;
   int nlocal = 0;

#define CHECKOFF(s, x) \
   efmt(s " expect OFF: %u ; ACTUAL %u\n", (uint)(x),(uint)(lseek(out->fd, 0, SEEK_CUR) + out->len))

   if (targ_64bit) {
      hdr.h64.shoff = alignup(reldataoff + reldatasize, align);
      hdr.h64.shnum = 11;
      hdr.h64.shstrndx = 5;
   } else {
      hdr.h32.shoff = alignup(reldataoff + reldatasize, align);
      hdr.h32.shnum = 11;
      hdr.h32.shstrndx = 5;
   }

   /* elf header */
   if (targ_64bit)
      elf64puthdr(out, &hdr.h64);
   else
      elf32puthdr(out, &hdr.h32);

   /* .text progbits */
   iowrite(out, O.textbegin, codesize);

   /* .rodata progbits */
   iowrite(out, O.rodata.p, O.rodata.n);

   /* .data progbits */
   iowrite(out, O.data.p, O.data.n);

   /* section names */
   iowrite(out, shstrs, sizeof shstrs);

   /* strings */
   iowrite(out, strs.p, strs.n);

   /* symtab */
   wordalign(out, align);
   for (int i = 0; i < symtab.n; ++i) {
      struct sym *sym = &symtab.p[i];
      if (sym->bind == STB_LOCAL) ++nlocal;
      putsym(out, sym);
   }

   /* rel.* */
   assert(relocs.n == ntextrel + nrodatarel + ndatarel);
   for (enum section s = Stext; s <= Sbss; ++s) {
      for (int i = 0; i < relocs.n; ++i) {
         struct reloc *rel = &relocs.p[i];
         if (rel->section != s) continue;
         putreloc(out, rel, userela);
      }
   }

   /** Section Headers **/
   wordalign(out, align);
#define putshdr(...) if (targ_64bit) elf64putshdr(out, &(struct elf64shdr) { __VA_ARGS__ }); \
                     else            elf32putshdr(out, &(struct elf32shdr) { __VA_ARGS__ });
   /* §0 null section */
   putshdr(0);
   /* §1 .text */
   putshdr(.name = shnam_text, .type = SHT_PROGBITS,
           .flags = SHF_ALLOC | SHF_EXECINSTR,
           .offset = targ_64bit ? hdr.h64.ehsize : hdr.h32.ehsize, .size = codesize,
           .addralign = align);
   /* §2 .rodata */
   putshdr(.name = shnam_rodata, .type = SHT_PROGBITS,
           .flags = SHF_ALLOC,
           .offset = rodataoff, .size = rodatasize,
           .addralign = O.rodataalign,);
   /* §3 .data */
   putshdr(.name = shnam_data, .type = SHT_PROGBITS,
           .flags = SHF_ALLOC | SHF_WRITE,
           .offset = dataoff, .size = datasize,
           .addralign = O.dataalign,);
   /* §4 .bss */
   putshdr(.name = shnam_bss, .type = SHT_NOBITS,
           .size = bsssize,
           .flags = SHF_ALLOC | SHF_WRITE,
           .addralign = O.bssalign,);
   /* §5 .shstrtab */
   putshdr(.name = shnam_shstrtab, .type = SHT_STRTAB,
           .offset = shstrsoff, .size = shstrssize,
           .flags = SHF_STRINGS );
   /* §6 .strtab */
   putshdr(.name = shnam_strtab, .type = SHT_STRTAB,
           .offset = strsoff, .size = strssize,
           .flags = SHF_STRINGS );
   /* §7 .symtab */
   putshdr(.name = shnam_symtab, .type = SHT_SYMTAB,
           .offset = symtaboff, .size = symtabsize,
           .flags = SHF_STRINGS,
           .link = 6, /* .strtab */
           .info = nlocal,
           .entsize = targ_64bit ? 24 : 16 );
   /* §8 .rel.text */
   putshdr(.name = shnam_reltext, .type = SHT_RELA,
           .offset = reltextoff, .size = reltextsize,
           .link = 7, /* .symtab */
           .entsize = relxsiz,
           .info = TEXT_SHNDX );
   /* §9 .rel.rodata */
   putshdr(.name = shnam_relrodata, .type = SHT_RELA,
           .offset = relrodataoff, .size = relrodatasize,
           .link = 7, /* .symtab */
           .entsize = relxsiz,
           .info = RODATA_SHNDX );
   /* §10 .rel.data */
   putshdr(.name = shnam_reldata, .type = SHT_RELA,
           .offset = reldataoff, .size = reldatasize,
           .link = 7, /* .symtab */
           .entsize = relxsiz,
           .info = DATA_SHNDX );
}
