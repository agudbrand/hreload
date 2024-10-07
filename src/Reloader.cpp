#include "Reloader.h"

#include "stdio.h"
#include "sys/mman.h"
#include "errno.h"
#include "unistd.h"
#include "dlfcn.h"
#include "string.h"

// TODO(sushi) write our own ELF parser eventually, this shit makes compiling
//             this file take like 3x longer than it should.
#include "elfio/elfio.hpp"
#include "elfio/elfio_symbols.hpp"
#include "elfio/elfio_dump.hpp"
#include "elf.h"
#include "link.h"

#include "iro/logger.h"
#include "iro/fs/file.h"
#include "iro/memory/allocator.h"

static Logger logger =
  Logger::create("reloader"_str, Logger::Verbosity::Info);

/* ============================================================================
 *  Helper for reading a loaded ELF file.
 */
struct ELF
{
  io::Memory buffer;

  /* --------------------------------------------------------------------------
   */
  b8 init(str path)
  {
    INFO("initializing ELF from path '", path, "'\n");
    
    auto file = fs::File::from(path, fs::OpenFlag::Read);
    if (isnil(file))
      return ERROR("failed to open file\n");
    defer { file.close(); };

    u64 file_size = file.getInfo().byte_size;

    if (!buffer.open(file_size))
      return false;

    if (file_size != buffer.consume(&file, file_size))
      return ERROR("failed to completely read file\n");

    auto header = (Elf64_Ehdr*)buffer.ptr;

    if (header->e_ident[EI_MAG0] != 0x7f || 
        header->e_ident[EI_MAG1] != 'E'  ||
        header->e_ident[EI_MAG2] != 'L'  ||
        header->e_ident[EI_MAG3] != 'F')
      return ERROR("invalid ELF header\n");

    if (header->e_ident[EI_CLASS] != ELFCLASS64)
      return ERROR("only 64bit ELF files are supported\n");

    // TODO(sushi) endianess or whatever ugh

    return true;
  }

  /* --------------------------------------------------------------------------
   */
  Elf64_Ehdr* operator->() { return getHeader(); }

  /* --------------------------------------------------------------------------
   */
  Elf64_Ehdr* getHeader() { return (Elf64_Ehdr*)buffer.ptr; } 

  /* ==========================================================================
   */
  struct ProgramHeaderEntry
  {
    Elf64_Phdr* header;

    Elf64_Phdr* operator->() { return header; }

    b8 isExecutable() { return header->p_flags & PF_X ;}
    b8 isWritable() { return header->p_flags & PF_W; }
    b8 isReadable() { return header->p_flags & PF_R; }
  };

  /* --------------------------------------------------------------------------
   */
  ProgramHeaderEntry getProgramHeader(u32 idx)
  {
    assert(idx < getHeader()->e_phnum);
    return ProgramHeaderEntry{
      (Elf64_Phdr*)(buffer.ptr + getHeader()->e_phoff) + idx};
  }

  /* ==========================================================================
   */
  struct Symbol
  {
    ELF* elf;
    Elf64_Sym* ptr;

    Elf64_Sym* operator->() { return ptr; }

    str getName() 
    { 
      if (ptr->st_name == 0)
        return nil;
      return elf->getSymbolStringTable().getString(ptr->st_name);
    }

    b8 hasType() { return ELF64_ST_TYPE(ptr->st_info) != STT_NOTYPE; }
    b8 isObject() { return ELF64_ST_TYPE(ptr->st_info) == STT_OBJECT; }
    b8 isFunc() { return ELF64_ST_TYPE(ptr->st_info) == STT_FUNC; }
    b8 isSection() { return ELF64_ST_TYPE(ptr->st_info) == STT_SECTION; }
    b8 isFile() { return ELF64_ST_TYPE(ptr->st_info) == STT_FILE; }

    b8 isLocal() { return ELF64_ST_BIND(ptr->st_info) == STB_LOCAL; }
    b8 isGlobal() { return ELF64_ST_BIND(ptr->st_info) == STB_GLOBAL; }
    b8 isWeak() { return ELF64_ST_BIND(ptr->st_info) == STB_WEAK; }

    b8 isDefaultVisibility() { return ptr->st_other == STV_DEFAULT; }
    b8 isHidden() { return ptr->st_other == STV_HIDDEN; }
    b8 isProtected() { return ptr->st_other == STV_PROTECTED; }
  };

  /* ==========================================================================
   */
  struct SectionHeaderEntry
  {
    ELF* elf;
    Elf64_Shdr* header;

    Elf64_Shdr* operator->() { return header; }

    b8 isNull() { return header->sh_type == SHT_NULL; }
    b8 isProgbits() { return header->sh_type == SHT_PROGBITS; }
    b8 isSymtab() { return header->sh_type == SHT_SYMTAB; }
    b8 isStrtab() { return header->sh_type == SHT_STRTAB; }
    b8 isRelocationsWithAddends() { return header->sh_type == SHT_RELA; }
    b8 isHashTable() { return header->sh_type == SHT_HASH; }
    b8 isDynamic() { return header->sh_type == SHT_DYNAMIC; }
    b8 isNobits() { return header->sh_type == SHT_NOBITS; }

    b8 shouldBeWritable() { return header->sh_flags & SHF_WRITE; }
    b8 occupiesMemory() { return header->sh_flags & SHF_ALLOC; }
    b8 containsInststructions() { return header->sh_flags & SHF_EXECINSTR; }

    str getName()
    {
      if (header->sh_name == 0)
        return nil;
      return elf->getSymbolStringTable().getString(header->sh_name);
    }

    u32 entCount() 
    {
      if (header->sh_entsize == 0)
        return 0;
      return header->sh_size / header->sh_entsize; 
    }

    u8* getStart() 
    { 
      assert(!isNobits());
      return elf->buffer.ptr + header->sh_offset; 
    }

    str getString(u32 idx)
    {
      assert(isStrtab() && idx);
      return str::fromCStr((char*)(getStart() + idx));
    }

    Symbol getEntry(u32 idx)
    {
      assert(!isNobits() && idx * header->sh_entsize < header->sh_size);
      return Symbol
      {
        elf,
        (Elf64_Sym*)(elf->buffer.ptr + header->sh_offset) + idx
      };
    }
  };

  /* --------------------------------------------------------------------------
   */
  SectionHeaderEntry getSectionHeader(u32 idx)
  {
    assert(idx < getHeader()->e_shnum);
    return SectionHeaderEntry
    {
      this,
      (Elf64_Shdr*)(buffer.ptr + getHeader()->e_shoff) + idx
    };
  }

  /* --------------------------------------------------------------------------
   */
  SectionHeaderEntry getSymbolStringTable()
  {
    return getSectionHeader(getHeader()->e_shstrndx);
  }
};

/* ============================================================================
 */
struct Reloader
{
  // Handle to the dynamic library loaded from the recompiled code.
  void* this_patch;
  void* prev_patch;

  u64 reload_idx;

  b8   init();
  void deinit();

  // libpath must be null-nerminated.
  b8 reloadSymbolsFromObjFile(
    const ReloadContext& context, 
    ReloadResult*        result);

  b8 redirectFunction(
    u8*        from, 
    void*      to,
    Remapping* remapping);

  b8 parseELF(str path);
};

/* ----------------------------------------------------------------------------
 */
b8 Reloader::init()
{
  iro::log.init();
  defer { iro::log.deinit(); };

  {
    using enum Log::Dest::Flag;
    Log::Dest::Flags flags;

    if (fs::stdout.isatty())
    {
      flags = 
          AllowColor
        | ShowVerbosity
        | PrefixNewlines;
    }
    else
    {
      flags = 
          ShowCategoryName
        | ShowVerbosity
        | PrefixNewlines;
    }

    iro::log.newDestination("stdout"_str, &fs::stdout, flags);
  }

  this_patch = nullptr;
  prev_patch = nullptr;
  reload_idx = 0;

  return true;
}

/* ----------------------------------------------------------------------------
 */
void Reloader::deinit()
{

}

/* ----------------------------------------------------------------------------
 */
b8 Reloader::reloadSymbolsFromObjFile(
    const ReloadContext& context,
    ReloadResult* result)
{
  using namespace ELFIO;

  str objpath = context.objpath;
  str exepath = context.exepath;

  if (prev_patch)
    dlclose(prev_patch);

  prev_patch = this_patch;

  io::StaticBuffer<512> patch_path_buf;
  patch_path_buf.open();

  io::formatv(&patch_path_buf, exepath, ".patch", reload_idx, ".so");

  str patchpath = patch_path_buf.asStr();

  reload_idx += 1;

  ELF elf;
  if (!elf.init(objpath))
    return ERROR("failed to initialize ELF file\n");

  for (u32 secidx = 0; secidx < elf->e_shnum; ++secidx)
  {
    auto sec = elf.getSectionHeader(secidx);
    if (sec.getName() == ".got"_str)
      printf("sec %s\n", sec.getName().ptr);
  }

  result->remappings_written = 0;

  return true;
}

/* ----------------------------------------------------------------------------
 */
b8 Reloader::redirectFunction(
    u8*        from, 
    void*      to,
    Remapping* remapping)
{
  errno = 0;

  remapping->kind = Remapping::Kind::Func;
  remapping->old_addr = from;
  remapping->new_addr = to;

  u8* bytes = remapping->func.bytes;

  int offset = 0;

  auto writeREX = [&](b8 w, b8 r, b8 x, b8 b)
  {
    bytes[offset] = 0x40 | (w << 3) | (r << 2) | (x << 1) | (b << 0);
    offset += 1;
  };

  auto writeByte = [&](u8 b)
  {
    bytes[offset] = b;
    offset += 1;
  };

  auto writeImm64 = [&](u64 x)
  {
    writeByte((x >>  0) & 0xff);
    writeByte((x >>  8) & 0xff);
    writeByte((x >> 16) & 0xff);
    writeByte((x >> 24) & 0xff);
    writeByte((x >> 32) & 0xff);
    writeByte((x >> 40) & 0xff);
    writeByte((x >> 48) & 0xff);
    writeByte((x >> 56) & 0xff);
  };

  // mov r11, &banana
  writeREX(true, false, false, true);
  writeByte(0xbb);
  writeImm64((u64)to);

  // jmp r11
  writeREX(false, false, false, true);
  writeByte(0xff);
  writeByte(0xe3);

  // ret
  writeByte(0xc3);

  return true;
}

/* ----------------------------------------------------------------------------
 */
extern "C" Reloader* hreloadCreateReloader()
{
  auto* r = mem::stl_allocator.allocateType<Reloader>();
  r->init();
  return r;
}

/* ----------------------------------------------------------------------------
 */
extern "C" b8 hreloadReloadSymbolsFromObjFile(
    Reloader*  r,
    const ReloadContext& context,
    ReloadResult* result)
{
  return r->reloadSymbolsFromObjFile(context, result);
}

/* ----------------------------------------------------------------------------
 */
extern "C" u64 hreloadPatchNumber(Reloader* r)
{
  return r->reload_idx;
}

/* ----------------------------------------------------------------------------
 */
extern "C" void test(u32* x)
{
  printf("test: %i\n", *x);
  *x = 11;
}
