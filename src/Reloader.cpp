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
#include "iro/containers/SmallArray.h"
#include "iro/containers/avl.h"
#include "iro/platform.h"

#include "gnu/lib-names.h"

static Logger logger =
  Logger::create("reloader"_str, Logger::Verbosity::Trace);

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

    b8 isNull() { return header->p_type == PT_NULL; }
    b8 isLoad() { return header->p_type == PT_LOAD; }
    b8 isDynamic() { return header->p_type == PT_DYNAMIC; }
    
  };

  /* --------------------------------------------------------------------------
   */
  ProgramHeaderEntry getProgramHeader(u32 idx)
  {
    assert(idx < getHeader()->e_phnum);
    return ProgramHeaderEntry{
      (Elf64_Phdr*)(buffer.ptr + getHeader()->e_phoff) + idx};
  }

  struct SectionHeaderEntry;

  /* ==========================================================================
   */
  struct Symbol
  {
    ELF* elf;
    u64 shidx; // Index of the section this symbol came from.
    Elf64_Sym* ptr;

    Elf64_Sym* operator->() { return ptr; }

    Symbol() : elf(nullptr), shidx(0), ptr(nullptr) {}
    Symbol(ELF* elf, u64 idx, Elf64_Sym* sym) 
      : elf(elf),
        shidx(idx),
        ptr(sym) {}

    b8 isValid() { return ptr != nullptr; }

    str getName() 
    { 
      if (ptr->st_name == 0)
        return nil;
      SectionHeaderEntry string_section;
      SectionHeaderEntry sym_section = elf->getSectionHeader(shidx);
      switch (sym_section.getType())
      {
      case SectionHeaderEntry::Type::DynamicSymbols:
      case SectionHeaderEntry::Type::Symtab:
      case SectionHeaderEntry::Type::Dynamic:
        string_section = elf->getSectionHeader(sym_section->sh_link);
        break;
      default:
        string_section = elf->getSymbolStringTable();
        break;
        
      }
      return string_section.getString(ptr->st_name);
    }

    b8 hasType() { return ELF64_ST_TYPE(ptr->st_info) != STT_NOTYPE; }
    b8 isObject() { return ELF64_ST_TYPE(ptr->st_info) == STT_OBJECT; }
    b8 isFunc() { return ELF64_ST_TYPE(ptr->st_info) == STT_FUNC; }
    b8 isSection() { return ELF64_ST_TYPE(ptr->st_info) == STT_SECTION; }
    b8 isFile() { return ELF64_ST_TYPE(ptr->st_info) == STT_FILE; }

    str getTypeString()
    {
      if (isObject())
        return "Object"_str;
      if (isFunc())
        return "Func"_str;
      if (isSection())
        return "Section"_str;
      if (isFile())
        return "File"_str;
      return "*Unknown*"_str;
    }


    b8 isLocal() { return ELF64_ST_BIND(ptr->st_info) == STB_LOCAL; }
    b8 isGlobal() { return ELF64_ST_BIND(ptr->st_info) == STB_GLOBAL; }
    b8 isWeak() { return ELF64_ST_BIND(ptr->st_info) == STB_WEAK; }

    str getBindString()
    {
      if (isLocal())
        return "Local"_str;
      if (isGlobal())
        return "Global"_str;
      if (isWeak())
        return "Weak"_str;
      return "*Unknown*"_str;
    }

    b8 isDefaultVisibility() { return ptr->st_other == STV_DEFAULT; }
    b8 isHidden() { return ptr->st_other == STV_HIDDEN; }
    b8 isProtected() { return ptr->st_other == STV_PROTECTED; }

    str getVisiblityString()
    {
      if (isDefaultVisibility())
        return "Default"_str;
      if (isHidden())
        return "Hidden"_str;
      if (isProtected())
        return "Protected"_str;
      return "*Unknown*"_str;
    }

    b8 isAbsolute() { return ptr->st_shndx == SHN_ABS; }

    void print()
    {
      TRACE(
          "Symbol: ", getName(), 
        "\n  value: ", ptr->st_value, "(", (void*)ptr->st_value, ")"
        "\n   size: ", ptr->st_size,
        "\n   type: ", getTypeString(),
        "\n   bind: ", getBindString(),
        "\n   visi: ", getVisiblityString(),
        "\n  shndx: ", ptr->st_shndx, "\n");
    }
  };

  /* ==========================================================================
   */
  struct Relocation
  {

  };

  /* ==========================================================================
   */
  struct SectionHeaderEntry
  {
    ELF* elf;
    Elf64_Shdr* header;
    u64 idx;

    Elf64_Shdr* operator->() { return header; }

    enum class Type
    {
      Unknown,
      Null,
      Progbits,
      Symtab,
      Strtab,
      RelocWithAddends,
      Hash,
      Dynamic,
      Note,
      Nobits,
      Reloc,
      DynamicSymbols,
    };

    Type getType()
    {
      switch (header->sh_type)
      {
      case SHT_NULL: return Type::Null;
      case SHT_PROGBITS: return Type::Progbits;
      case SHT_SYMTAB: return Type::Symtab;
      case SHT_STRTAB: return Type::Strtab;
      case SHT_RELA: return Type::RelocWithAddends;
      case SHT_HASH: return Type::Hash;
      case SHT_DYNAMIC: return Type::Dynamic;
      case SHT_NOTE: return Type::Note;
      case SHT_NOBITS: return Type::Nobits;
      case SHT_REL: return Type::Reloc;
      case SHT_DYNSYM: return Type::DynamicSymbols;
      default: return Type::Unknown;
      }
    }

    b8 isType(Type type)
    {
      return getType() == type;
    }

    b8 isNull() { return isType(Type::Null); }
    b8 isProgbits() { return isType(Type::Progbits); }
    b8 isSymtab() { return isType(Type::Symtab); }
    b8 isStrtab() { return isType(Type::Strtab); }
    b8 isRelocsWithAddends() { return isType(Type::RelocWithAddends); }
    b8 isHashTable() { return isType(Type::Hash); }
    b8 isDynamic() { return isType(Type::Dynamic); }
    b8 isNobits() { return isType(Type::Nobits); }
    b8 isReloc() { return isType(Type::Reloc); }

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

    Symbol getEntry(u32 entidx)
    {
      assert(!isNobits() && entidx * header->sh_entsize < header->sh_size);
      return Symbol(
        elf,
        idx,
        (Elf64_Sym*)(
          elf->buffer.ptr + header->sh_offset + entidx * header->sh_entsize));
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
      (Elf64_Shdr*)(buffer.ptr + getHeader()->e_shoff) + idx,
      idx
    };
  }

  /* --------------------------------------------------------------------------
   */
  SectionHeaderEntry getSymbolStringTable()
  {
    return getSectionHeader(getHeader()->e_shstrndx);
  }

  /* --------------------------------------------------------------------------
   */
  Symbol findSymbol(str name)
  {
    for (u32 si = 0; si < getHeader()->e_shnum; ++si)
    {
      auto sec = getSectionHeader(si);
      if (sec.isSymtab())
      {
        for (u32 ei = 0; ei < sec.entCount(); ++ei)
        {
          auto ent = sec.getEntry(ei);
          str ename = ent.getName();
          if (notnil(name))
          {
            if (ename == name)
              return ent;
          }
        }
      }
    }
    return Symbol();
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

  b8 init()
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

  void deinit()
  {
    
  }

  b8 collectSyms(StringSet* set, str path)
  {
    ELF elf;
    if (!elf.init(path))
      return ERROR("failed to initialize ELF for '", path, "'\n");

    for (u32 secidx = 0; secidx < elf->e_shnum; ++secidx)
    {
      auto sec = elf.getSectionHeader(secidx);
      switch (sec.getType())
      {
        using SHType = ELF::SectionHeaderEntry::Type;
        case SHType::DynamicSymbols:
        case SHType::Symtab:
        for (u32 i = 0; i < sec.entCount(); ++i)
        {
          auto ent = sec.getEntry(i);
          if (ent->st_name != 0)
            set->add(ent.getName());
        }
        break;
      }
    }

    return true;
  }

  // libpath must be null-nerminated.
  b8 reloadSymbolsFromObjFile(
    const ReloadContext& context, 
    ReloadResult*        result)
  {
    using namespace ELFIO;

    str odefpath = context.odefpath;
    str exepath = context.exepath;

    // if (prev_patch)
    //   dlclose(prev_patch);

    prev_patch = this_patch;
    if (!prev_patch)
      prev_patch = context.reloadee_handle;

    io::StaticBuffer<512> patch_path_buf;
    patch_path_buf.open();

    io::StaticBuffer<512> prev_patch_path_buf;
    prev_patch_path_buf.open();

    void* libc = dlopen(LIBC_SO, RTLD_LAZY);

    struct link_map* lm;
    dlinfo(libc, RTLD_DI_LINKMAP, &lm);

    StringSet libc_symbols;
    if (!libc_symbols.init())
      return false;
    defer { libc_symbols.deinit(); };

    // Collect symbols from libc so that we can filter them out.
    if (!collectSyms(&libc_symbols, str::fromCStr(lm->l_name)))
      return ERROR("failed to collect libc symbols\n");

    io::formatv(&patch_path_buf, exepath, ".patch", reload_idx, ".so");

    if (reload_idx == 0)
      io::formatv(&prev_patch_path_buf, exepath);
    else
      io::formatv(
          &prev_patch_path_buf, exepath, ".patch", reload_idx-1, ".so");

    str patchpath = patch_path_buf.asStr();

    this_patch = dlopen((char*)patch_path_buf.asStr().ptr, RTLD_LAZY);

    TRACE("prev: ", prev_patch, " this: ", this_patch, "\n");

    dlinfo(this_patch, RTLD_DI_LINKMAP, &lm);
    void* this_start_addr = (void*)lm->l_addr;
    char* this_elf_path = lm->l_name;
    TRACE("this_patch: ", lm->l_name, "\n");
    dlinfo(prev_patch, RTLD_DI_LINKMAP, &lm);
    void* prev_start_addr = (void*)lm->l_addr;
    char* prev_elf_path = lm->l_name;
    TRACE("prev_patch: ", lm->l_name, "\n");

    TRACE("prev start: ", prev_start_addr, "\n"
         "this_start_addr: ", this_start_addr, "\n");

    TRACE("patch path: ", patch_path_buf.asStr(), "\n"
         "this elf  : ", this_elf_path, "\n");

    ELF this_elf;
    if (!this_elf.init(str::fromCStr(this_elf_path)))
      return ERROR("failed to read ELF of patched executable\n");

    auto odef_file = fs::File::from(odefpath, fs::OpenFlag::Read);
    if (isnil(odef_file))
      return ERROR("failed to open odefs at path '", odefpath, "'\n");
    defer { odef_file.close(); };

    u64 odefs_size = odef_file.getInfo().byte_size;

    io::Memory odefs;
    if (!odefs.open(odefs_size))
      return ERROR("failed to open buffer for reading odefs\n");
    defer { odefs.close(); };

    if (odefs_size != odefs.consume(&odef_file, odefs_size))
      return ERROR("failed to read entire odefs file\n");

    SmallArray<str, 64> odef_paths;
    if (!odef_paths.init())
      return ERROR("failed to initialize array for odefs\n");
    defer { odef_paths.deinit(); };

    str scan = odefs.asStr();
    for (;;)
    {
      u8* nl_or_eof = scan.ptr;
      while (!matchAny(*nl_or_eof, '\n', 0))
        nl_or_eof += 1;
      str odef = str::from(scan.ptr, nl_or_eof);
      if (!odef.isEmpty())
        odef_paths.push(odef);
      if (*nl_or_eof == 0)
        break;
      scan = str::from(nl_or_eof + 1, scan.end());
    }

    reload_idx += 1;

    result->remappings_written = 0;

    StringSet reloaded_symbols;
    if (!reloaded_symbols.init())
      return ERROR("failed to init reloaded symbols set\n");
    defer { reloaded_symbols.deinit(); };

    for (str& path : odef_paths)
      if (!collectSyms(&reloaded_symbols, path))
        return false;

    auto printSyms = [&](str path)
    {
      ELF elf;
      if (!elf.init(path))
        return ERROR("failed to initialize ELF file\n");

      for (u32 secidx = 0; secidx < elf->e_shnum; ++secidx)
      {
        auto sec = elf.getSectionHeader(secidx);
        TRACE("sec %s has %d entries at %p:\n", 
            sec.getName().ptr, 
            sec.entCount(),
            (void*)sec->sh_addr);
        switch (sec.getType())
        {
          using SHType = ELF::SectionHeaderEntry::Type;
          case SHType::Null:
          TRACE("  null\n");
          break;

          case SHType::Progbits:
          TRACE("  progbits\n");
          break;

          case SHType::Strtab:
          TRACE("  string table:\n");
          if (false)
          {
            str s = sec.getString(1);
            u8* start = s.ptr;
            for (;;)
            {
              TRACE("    %d %s\n", u32(s.ptr - start), s.ptr);
              if (s.end()[1] == 0)
                break;
              s = str::fromCStr(s.end() + 1);
            }
          }
          break;

          case SHType::DynamicSymbols:
          case SHType::Symtab:
          TRACE("  symtab with %d entries:\n", sec.entCount());
          for (u32 i = 0; i < sec.entCount(); ++i)
          {
            auto ent = sec.getEntry(i);
            if (!ent.isFunc() && !ent.isObject())
              continue;
            str shidx_name = nil;
            if (ent->st_shndx != SHN_UNDEF && ent->st_shndx < SHN_LORESERVE)
              shidx_name = elf.getSectionHeader(ent->st_shndx).getName();
            else
              shidx_name = "undef"_str;

            if (reloaded_symbols.has(ent.getName()) &&
                !libc_symbols.has(ent.getName()))
            {
              ent.print();
              Remapping* remapping = 
                &context.remappings[result->remappings_written];

              const char* name = (char*)ent.getName().ptr;

              if (ent.isFunc())
              {
                void* prev_addr = dlsym(prev_patch, name);
                void* this_addr = dlsym(this_patch, name);

                if (prev_addr && this_addr && prev_addr != this_addr)
                {
                  if (!redirectFunction(
                        prev_addr,
                        this_addr,
                        remapping))
                    return false;
                  result->remappings_written += 1;
                  TRACE("    ", ent.getName(), " redirect\n");
                  TRACE("    ", prev_addr, " -> ", this_addr, "\n");
                }
              }
              else if (ent.isGlobal())
              {
                TRACE("--- global ---\n");
              }
              else if (ent.isLocal())
              {
                ELF::Symbol this_ent = 
                  this_elf.findSymbol(str::fromCStr(name));

                if (this_ent.isValid() && !ent.isAbsolute())
                {
                  auto sec = elf.getSectionHeader(ent->st_shndx);
                  if (sec.shouldBeWritable())
                  {
                    TRACE("----\nnew ent:\n");
                    this_ent.print();
                    Remapping* remapping = 
                      &context.remappings[result->remappings_written];
                    result->remappings_written += 1;
                    remapping->kind = Remapping::Kind::Mem;
                    remapping->mem.size = ent->st_size;
                    remapping->old_addr = (u8*)prev_start_addr + ent->st_value;
                    remapping->new_addr = 
                      (u8*)this_start_addr + this_ent->st_value;
                    TRACE(this_ent.getName(), " => ", ent.getName(), "\n",
                         remapping->old_addr, " -> ", remapping->new_addr, 
                         "\n");
                  }
                }
              }
            }
            else
              TRACE("   not reloading ", ent.getName().ptr, "\n");
          }
          break;
        }
      }

      return true;
    };


    for (str sym : reloaded_symbols)
      TRACE("s: %s\n", sym.ptr);

    if (!printSyms(prev_patch_path_buf.asStr()))
      return false;

    return true;

  }

  b8 redirectFunction(
    void*      from, 
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

    return true;

  }
};

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
extern "C" void copyLog(iro::Log* log)
{
  mem::copy(&iro::log, log, sizeof(iro::Log));
}

/* ----------------------------------------------------------------------------
 */
extern "C" void test(Array<u32>* x)
{
  for (u32 i = 0; i < 100; ++i)
    x->push(i);
}
