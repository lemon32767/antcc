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

struct elfhdr {
   uchar  i_mag[4],
          i_class,
          i_data,
          i_version,
          i_osabi,
          i_abiversion,
          i_pad[7];
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

struct elfshdr {
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
struct elfsym {
   uint   name;
   uchar  info,
          other;
   ushort shndx;
   uvlong value,
          size;
};

#define ELF_R_INFO(s,t) ((uvlong) (s) << 32 | (t))
struct elfrel {
   uvlong offset, info;
};

struct elfrela {
   uvlong offset, info;
   vlong addend;
};

struct elfphdr {
   uint   type,
          flags;
   uvlong offset,
          vaddr,
          paddr,
          filesz,
          memsz,
          align;
};

/* vim:set ts=3 sw=3 expandtab: */
