#include "common.h"

#define ELFMAG "\177ELF"
enum {
   ELFCLASS32 = 1,
   ELFCLASS64 = 2,

   ELFDATA2LSB = 1,
   ELFDATA2MSB = 2,

   ELFVERSION = 1,

   ELFOSABI_SYSV       = 0,
   ELFOSABI_ARM        = 97,
   ELFOSABI_STANDALONE = 255,

   ET_NONE = 0,
   ET_REL, ET_EXEC, ET_DYN, ET_CORE,

   EM_NONE         = 0,
   EM_386          = 3,
   EM_486          = 6,
   EM_MIPS         = 8,
   EM_MIPS_RS4_BE  = 0xA,
   EM_ARM          = 0x28,
   EM_X86_64       = 0x3E,
   EM_ARM64        = 0xB7,
};

#define ELF_HDRIDENT          \
   union {                    \
      uchar ident[16];        \
      struct {                \
         uchar  i_mag[4],     \
                i_class,      \
                i_data,       \
                i_version,    \
                i_osabi,      \
                i_abiversion, \
                i_pad[7];     \
      };                      \
   }

struct elf64hdr {
   ELF_HDRIDENT;
   ushort type,
          machine;
   uint   version;
   uvlong entry,
          phoff,
          shoff;
   uint   flags;
   ushort ehsize,
          phentsize,
          phnum,
          shentsize,
          shnum,
          shstrndx;
};
static_assert(sizeof(struct elf64hdr) == 64);

struct elf32hdr {
   ELF_HDRIDENT;
   ushort type,
          machine;
   uint   version;
   uint   entry,
          phoff,
          shoff;
   uint   flags;
   ushort ehsize,
          phentsize,
          phnum,
          shentsize,
          shnum,
          shstrndx;
};
static_assert(sizeof(struct elf32hdr) == 52);

enum {
   SHT_NULL          = 0x0,
   SHT_PROGBITS      = 0x1,
   SHT_SYMTAB        = 0x2,
   SHT_STRTAB        = 0x3,
   SHT_RELA          = 0x4,
   SHT_HASH          = 0x5,
   SHT_DYNAMIC       = 0x6,
   SHT_NOTE          = 0x7,
   SHT_NOBITS        = 0x8,
   SHT_REL           = 0x9,
   SHT_SHLIB         = 0xA,
   SHT_DYNSYM        = 0xB,
   SHT_INIT_ARRAY    = 0xE,
   SHT_FINI_ARRAY    = 0xF,
   SHT_PREINIT_ARRAY = 0x10,
   SHT_GROUP         = 0x12,
   SHT_SYMTAB_SHNDX  = 0x13,
};

enum {
   SHF_WRITE            = 0x1,
   SHF_ALLOC            = 0x2,
   SHF_EXECINSTR        = 0x4,
   SHF_MERGE            = 0x10,
   SHF_STRINGS          = 0x20,
   SHF_INFO_LINK        = 0x40,
   SHF_LINK_ORDER       = 0x80,
   SHF_OS_NONCONFORMING = 0x100,
   SHF_GROUP            = 0x200,
   SHF_TLS              = 0x400,
};

struct elf64shdr {
   uint   name,
          type;
   uvlong flags,
          addr,
          offset,
          size;
   uint   link,
          info;
   uvlong addralign,
          entsize;
};
static_assert(sizeof(struct elf64shdr) == 64);

struct elf32shdr {
   uint name,
        type,
        flags,
        addr,
        offset,
        size,
        link,
        info,
        addralign,
        entsize;
};
static_assert(sizeof(struct elf32shdr) == 40);

enum {
   STB_LOCAL,
   STB_GLOBAL,
   STB_WEAK
};

enum {
   STT_NOTYPE,
   STT_OBJECT,
   STT_FUNC,
   STT_SECTION,
   STT_FILE,
};

enum {
   SHN_UND = 0,
   SHN_ABS = 0xFFF1,
};

#define ELF_S_INFO(b,t) ((b) << 4 | (t))

struct elf64sym {
   uint   name;
   uchar  info,
          other;
   ushort shndx;
   uvlong value,
          size;
};
static_assert(sizeof(struct elf64sym) == 24);

struct elf32sym {
   uint   name,
          value,
          size;
   uchar  info,
          other;
   ushort shndx;
};
static_assert(sizeof(struct elf32sym) == 16);

#define ELF64_R_INFO(s,t) ((uvlong) (s) << 32 | (uint)(t))
struct elf64rel {
   uvlong offset, info;
};
static_assert(sizeof(struct elf64rel) == 16);

#define ELF32_R_INFO(s,t) ((s) << 8 | (uchar)(t))
struct elf32rel {
   uint offset, info;
};
static_assert(sizeof(struct elf32rel) == 8);

struct elf64rela {
   uvlong offset, info;
   vlong addend;
};
static_assert(sizeof(struct elf64rela) == 24);

struct elf32rela {
   uint offset, info;
   int addend;
};
static_assert(sizeof(struct elf32rela) == 12);

/* vim:set ts=3 sw=3 expandtab: */
