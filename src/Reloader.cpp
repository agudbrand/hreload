/*
 *  The Reloader type used to perform patching at runtime.
 *
 *  IMPORTANT!!!!
 *
 *  DO NOT change ANY global state inside of the patching functions. An example 
 *  of this is using SCOPED_INDENT. If this is statically linked, we will be 
 *  using the same log global as the user (if the project uses iro's logger)
 *  and we'll increment the indentation count on the OLD log global. This will
 *  be copied to the NEW log global, but the scoped indent is going to deindent
 *  the OLD log global, leaving us with a new indentation level everytime we 
 *  hot reload.
 *
 *  It is probably best this lib be dynamically linked to avoid this but I 
 *  don't like doing that so for now I'm gonna leave it statically linked 
 *  and hope it doesn't cause any horrible issues down the line.
 */

#include "Reloader.h"

#include "stdio.h"
#include "sys/mman.h"
#include "errno.h"
#include "unistd.h"
#include "dlfcn.h"
#include "string.h"

#include "elf.h"
#include "link.h"

#include "iro/logger.h"
#include "iro/fs/file.h"
#include "iro/memory/allocator.h"
#include "iro/containers/SmallArray.h"
#include "iro/containers/avl.h"
#include "iro/platform.h"
#include "iro/memory/bump.h"

#include "gnu/lib-names.h"

namespace hr
{

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
  b8 init(String path, mem::Allocator* allocator)
  {
    DEBUG("initializing ELF from path '", path, "'\n");
    
    auto file = fs::File::from(path, fs::OpenFlag::Read);
    if (isnil(file))
      return ERROR("failed to open file\n");
    defer { file.close(); };

    u64 file_size = file.getInfo().byte_size;

    if (!buffer.open(file_size, allocator))
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
  void deinit()
  {
    buffer.close();
  }

  /* --------------------------------------------------------------------------
   */
  Elf64_Ehdr* operator->() const { return getHeader(); }

  /* --------------------------------------------------------------------------
   */
  Elf64_Ehdr* getHeader() const { return (Elf64_Ehdr*)buffer.ptr; } 
  u16 getSectionHeaderCount() { return getHeader()->e_shnum; }

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
    const ELF* elf;
    u64 shidx; // Index of the section this symbol came from.
    Elf64_Sym* ptr;

    Elf64_Sym* operator->() { return ptr; }

    Symbol() : elf(nullptr), shidx(0), ptr(nullptr) {}
    Symbol(const ELF* elf, u64 idx, Elf64_Sym* sym) 
      : elf(elf),
        shidx(idx),
        ptr(sym) {}

    b8 isValid() { return ptr != nullptr; }

    String getName() 
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

    String getTypeString()
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

    String getBindString()
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

    b8 isDefined() { return ptr->st_shndx != SHN_UNDEF; }

    String getVisiblityString()
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
        "\n   type: ", getTypeString(), " (", ELF64_ST_TYPE(ptr->st_info), ")",
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
    const ELF* elf;
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
    b8 isDynamicSymbols() { return isType(Type::DynamicSymbols); }
    b8 isNobits() { return isType(Type::Nobits); }
    b8 isReloc() { return isType(Type::Reloc); }

    b8 shouldBeWritable() { return header->sh_flags & SHF_WRITE; }
    b8 occupiesMemory() { return header->sh_flags & SHF_ALLOC; }
    b8 containsInststructions() { return header->sh_flags & SHF_EXECINSTR; }

    String getName()
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

    String getString(u32 idx)
    {
      assert(isStrtab() && idx && idx < header->sh_size);
      return String::fromCStr((char*)(getStart() + idx));
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
  SectionHeaderEntry getSectionHeader(u32 idx) const
  {
    assert(idx < getHeader()->e_shnum);
    return SectionHeaderEntry
    {
      this,
      (Elf64_Shdr*)(buffer.ptr + getHeader()->e_shoff + 
          idx * getHeader()->e_shentsize),
      idx
    };
  }

  /* --------------------------------------------------------------------------
   */
  SectionHeaderEntry getSymbolStringTable() const
  {
    return getSectionHeader(getHeader()->e_shstrndx);
  }

  /* --------------------------------------------------------------------------
   */
  Symbol findSymbol(String name) const
  {
    for (u32 si = 0; si < getHeader()->e_shnum; ++si)
    {
      auto sec = getSectionHeader(si);
      if (sec.isSymtab())
      {
        for (u32 ei = 0; ei < sec.entCount(); ++ei)
        {
          auto ent = sec.getEntry(ei);
          String ename = ent.getName();
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

  // Cleared at the end of a reload.
  mem::LenientBump talloc;

  struct Patch
  {
    // The elf file representing this Patch.
    ELF elf = {};

    // Where in memory this Patch's ELF file resides.
    void* start_addr = nullptr;

    // Handle to this patch's dl stuff.
    void* dhandle = nullptr;

    b8 isValid() const 
    { 
      return start_addr != nullptr &&
             dhandle != nullptr; 
    }

    /* ------------------------------------------------------------------------
     */
    b8 init(mem::Allocator* allocator, String path)
    {
      DEBUG("initializing patch for '", path, "'\n");
      if (!elf.init(path, allocator))
        return ERROR("failed to initialize patch ELF\n");

      dlerror();

      dhandle = dlopen((char*)path.ptr, RTLD_LAZY);

      if (dhandle == nullptr)
        return ERROR("failed to dlopen patch: ", dlerror(), "\n");

      struct link_map* lm;
      dlinfo(dhandle, RTLD_DI_LINKMAP, &lm);

      start_addr = (void*)lm->l_addr;

      assert(isValid());

      return true;
    }

    /* ------------------------------------------------------------------------
     */
    b8 initBase(mem::Allocator* allocator, void* dhandle, String exepath)
    {
      DEBUG("initializing patch for dl handle '", dhandle, "'\n");

      this->dhandle = dhandle;

      struct link_map* lm;
      dlinfo(dhandle, RTLD_DI_LINKMAP, &lm);

      start_addr = (void*)lm->l_addr;

      if (!elf.init(exepath, allocator))
        return ERROR("failed to initialize patch ELF\n");

      return true;
    }

    /* ------------------------------------------------------------------------
     */
    void deinit()
    {
      elf.deinit();
      dlclose(dhandle);
      start_addr = dhandle = nullptr;
    }

    /* ------------------------------------------------------------------------
     */
    void moveTo(Patch* rhs)
    {
      rhs->elf = elf;
      rhs->dhandle = dhandle;
      rhs->start_addr = start_addr;
      elf = {};
      start_addr = dhandle = nullptr;
    }

    /* ------------------------------------------------------------------------
     *  Iterate the symbols in this patch's ELF and redirect any functions 
     *  with a matching function in 'to's ELF if they are specified to be 
     *  reloaded and if they aren't filtered.
     *
     *  TODO(sushi) can just filter out the symbols from reloaded symbols to 
     *              avoid using two maps here.
     */
    b8 redirectFunctionsTo(
        const Patch& to, 
        const StringSet& patchable_symbols)
    {
      assert(isValid() && to.isValid());

      u64 remappings_written = 0;

      for (u32 seci = 0; seci < elf.getSectionHeaderCount(); ++seci)
      {
        auto sec = elf.getSectionHeader(seci);
        if (!sec.isSymtab())
          continue;

        for (u32 i = 0; i < sec.entCount(); ++i)
        {
          auto from_ent = sec.getEntry(i);
          if (!from_ent.isFunc() || !from_ent.isDefined())
            continue;

          String from_name = from_ent.getName();
          if (isnil(from_name))
            // Can't match to any other symbol nor filter.
            continue;
          
          // Filter out global initializer functions.
          if (from_name.startsWith("_GLOBAL__sub_I_"_str) || // static globals
              from_name.startsWith("__cxx_global_var_init"_str))
            continue;

          if (from_name.endsWith(".ro"_str))
            continue;

          if (!patchable_symbols.has(from_name))
            continue;

          // Lookup the symbol in the patch we are redirecting functions to.
          auto to_ent = to.elf.findSymbol(from_name);
          if (!to_ent.isValid())
            // The new patch no longer has this symbol (probably).
            continue;

          void* from_addr = (u8*)start_addr + from_ent->st_value;
          void* to_addr = (u8*)to.start_addr + to_ent->st_value;

          DEBUG("patching ", from_name, ": \n",
               "  ", from_addr, " => ", to_addr, "\n");
          from_ent.print();
          to_ent.print();

          void* check = dlsym(to.dhandle, (char*)from_name.ptr);
          if (check && check != to_addr)
            platform::debugBreak();

          void* aligned = (void*)((u64)from_addr & -getpagesize());
          if (mprotect(
                aligned, 
                getpagesize(), 
                PROT_EXEC | PROT_READ | PROT_WRITE))
            return ERROR("failed to mprotect ", aligned, ": ", 
                         strerror(errno), "\n");

          u8* bytes = (u8*)from_addr;
          u64 offset = 0;
          auto writeByte = [&](u8 b)
          {
            bytes[offset] = b;
            offset += 1;
          };

          // mov r11, <from_addr>
          writeByte(0x40 | (1 << 3) | (1 << 0)); // REX.WB prefix
          writeByte(0xbb); // opcode
          // imm64 of <from_addr>
          writeByte((u64(to_addr) >>  0) & 0xff);
          writeByte((u64(to_addr) >>  8) & 0xff);
          writeByte((u64(to_addr) >> 16) & 0xff);
          writeByte((u64(to_addr) >> 24) & 0xff);
          writeByte((u64(to_addr) >> 32) & 0xff);
          writeByte((u64(to_addr) >> 40) & 0xff);
          writeByte((u64(to_addr) >> 48) & 0xff);
          writeByte((u64(to_addr) >> 56) & 0xff);

          // jmp r11
          writeByte(0x40 | (1 << 0)); // REX.B prefix
          writeByte(0xff); // opcode?
          writeByte(0xe3); // idk.
        }
      }

      return true;
    }

    /* ------------------------------------------------------------------------
     *  Iterate the symbols in this patch and copy the state of globals 
     *  to the new patch's globals. I really don't think is a proper way to 
     *  handle this and will likely break!
     */
    b8 copyGlobalState(const Patch& to, const StringSet& patchable_symbols)
    {
      assert(isValid() && to.isValid());

      u64 remappings_written = 0;

      for (u32 seci = 0; seci < elf.getSectionHeaderCount(); ++seci)
      {
        auto sec = elf.getSectionHeader(seci);
        if (!sec.isSymtab() && !sec.isDynamicSymbols())
          continue;

        for (u32 i = 0; i < sec.entCount(); ++i)
        {
          auto from_ent = sec.getEntry(i);
          TRACE("ent: ", from_ent.getName(), "\n");
          if (!from_ent.isObject())
          {
            TRACE("cannot patch because this entry is not an object\n");
            continue;
          }

          if (from_ent.isAbsolute())
          {
            TRACE("cannot patch because this entry is absolute\n");
            continue;
          }

          // Make sure this isn't in a readonly section.
          String ent_sec = elf.getSectionHeader(from_ent->st_shndx).getName();

          if (ent_sec == ".rodata"_str)
          {
            TRACE(
              "cannot patch because this entry is in a readonly section\n");
            continue;
          }

          String from_name = from_ent.getName();
          if (isnil(from_name))
          {
            TRACE("cannot patch because this entry does not have a name\n");
            continue;
          }

          if (from_name.startsWith(".L"_str))
          {
            TRACE("cannot patch because this is a string literal\n");
            continue;
          }

          String sec_name = elf.getSectionHeader(from_ent->st_shndx).getName();
          if (sec_name.endsWith(".ro"_str))
          {
            TRACE("cannot patch because this is in a read-only section\n");
            continue;
          }

          if (!patchable_symbols.has(from_name))
          {
            TRACE("cannot patch ", from_name, " because it was filtered\n");
            continue;
          }

          auto to_ent = to.elf.findSymbol(from_name);
          if (!to_ent.isValid())
          {
            TRACE("cannot patch ", from_name, " because it could not be "
                  "found in target patch\n");
            continue;
          }

          void* from_addr = (u8*)start_addr + from_ent->st_value;
          void* to_addr = (u8*)to.start_addr + to_ent->st_value;

          DEBUG("copyGlobalState: ", from_addr, " -> ", to_addr, " ", 
               from_name, "\n",
               "  sec: ", 
                elf.getSectionHeader(from_ent->st_shndx).getName(), "\n");

          void* aligned = (void*)((u64)to_addr & -getpagesize());
          if (mprotect(
                aligned, 
                2*getpagesize(), 
                PROT_EXEC | PROT_READ | PROT_WRITE))
            return ERROR("failed to mprotect ", aligned, ": ", 
                         strerror(errno), "\n");

          mem::copy(to_addr, from_addr, from_ent->st_size);
        }
      }

      return true;
    }
  };

  struct
  {
    Patch base;
    Patch prev;
    Patch curr;
  } patch = {};

  /* --------------------------------------------------------------------------
   */
  b8 init()
  {
    this_patch = nullptr;
    prev_patch = nullptr;
    reload_idx = 0;

    return true;
  }

  /* --------------------------------------------------------------------------
   */
  void deinit()
  {
    talloc.deinit();
    patch.base.deinit();
    patch.prev.deinit();
    patch.curr.deinit();
  }

  /* --------------------------------------------------------------------------
   */
  b8 collectSyms(
      const ELF& elf, 
      StringSet* set,
      const StringSet* filtered)
  {
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
          String name = ent.getName();
          if (notnil(name) && (!filtered || !filtered->has(name)))
            set->add(name);
        }
        break;
      }
    }

    return true;
  }

  /* --------------------------------------------------------------------------
   *  It is expected that the ELF allocated here is cleaned up elsewhere. We
   *  don't clean it up here because that would invalidate the string data
   *  we need to read later, eg. in the case of collecting symbols for 
   *  filtering.
   */
  b8 collectSyms(
      mem::Allocator* allocator, 
      StringSet* set, 
      const StringSet* filtered,
      String path)
  {
    ELF elf;
    if (!elf.init(path, allocator))
      return ERROR("failed to initialize ELF for '", path, "'\n");

    return collectSyms(elf, set, filtered);
  }

  /* --------------------------------------------------------------------------
   *  Collect symbols we know we never want to filter, eg. libc and such.
   */
  b8 collectDefaultFilteredSymbols(
      mem::Allocator* allocator, 
      StringSet* filtered)
  {
    // libc
    {
      // Find the correct path to libc via a gcc thing. This is probably 
      // NOT portable!
      void* libc = dlopen(LIBC_SO, RTLD_LAZY);

      struct link_map* lm;
      dlinfo(libc, RTLD_DI_LINKMAP, &lm);

      if (!collectSyms(
            allocator, 
            filtered, 
            nullptr,
            String::fromCStr(lm->l_name)))
        return ERROR("failed to collect filtered symbols from libc\n");
    }

    return true;
  }

  /* --------------------------------------------------------------------------
   *  Collect symbols from 'path' to filter, unless if they already exist in 
   *  the patchable symbols set.
   */
  b8 collectFilteredSymbols(
      mem::Allocator* allocator, 
      String path,
      const StringSet& patchable,
      StringSet* filtered)
  {
    return 
      collectSyms(
        allocator,
        filtered,
        &patchable,
        path);
  }

  /* --------------------------------------------------------------------------
   */
  b8 collectPatchableSymbols(
      mem::Allocator* allocator,
      StringSet* patchable,
      StringSet* filtered,
      String hrfpath)
  {
    auto hrf_file = fs::File::from(hrfpath, fs::OpenFlag::Read);
    if (isnil(hrf_file))
      return ERROR("failed to open hrfs at path '", hrfpath, "'\n");
    defer { hrf_file.close(); };

    u64 hrfs_size = hrf_file.getInfo().byte_size;

    io::Memory hrfs;
    if (!hrfs.open(hrfs_size))
      return ERROR("failed to open buffer for reading hrfs\n");
    defer { hrfs.close(); };

    if (hrfs_size != hrfs.consume(&hrf_file, hrfs_size))
      return ERROR("failed to read entire hrfs file\n");

    String scan = hrfs.asStr();
    for (;;)
    {
      u8* nl_or_eof = scan.ptr;
      while (!matchAny(*nl_or_eof, '\n', 0))
        nl_or_eof += 1;
      String filter = String::from(scan.ptr, nl_or_eof);
      if (!filter.isEmpty())
      {
        if (filter.startsWith("+o"_str))
        {
          String path = filter.sub(2);
          if (!collectSyms(allocator, patchable, filtered, path))
            return ERROR("failed to collect symbols for file '", path, "'\n");
        }
        else if (filter.startsWith("-o"_str))
        {
          String path = filter.sub(2);
          if (!collectFilteredSymbols(allocator, path, *patchable, filtered))
            return ERROR("failed to collect filtered symbols from file '", 
                         path, "'\n");
        }
        else if (filter.startsWith("-l"_str))
        {
          String lib = filter.sub(2);
          if (lib.startsWith('/'))
          {
            // Must be an absolute path.
            if (!collectSyms(allocator, patchable, filtered, lib))
              return ERROR("failed to collect symbols for file '", lib, "\n");
          }
          else
          {
            // Must be the name of the library, so try to find it by asking 
            // the dynamic linker to open it and give us its path.

            io::StaticBuffer<512> libname;
            io::formatv(&libname, "lib", lib, ".so");

            void* lib_handle = dlopen((char*)libname.asStr().ptr, RTLD_LAZY);

            struct link_map* lm;
            dlinfo(lib_handle, RTLD_DI_LINKMAP, &lm);

            if (lm->l_name == nullptr || lm->l_name[0] == '\0')
              return ERROR("failed to get path to library '", lib, "'\n");

            if (!collectFilteredSymbols(
                  allocator,
                  String::fromCStr(lm->l_name),
                  *patchable,
                  filtered)) 
              return ERROR("failed to collect symbols for file '", lib, "'\n");
          }
        }
      }
      if (*nl_or_eof == 0)
        break;
      scan = String::from(nl_or_eof + 1, scan.end());
    }

    return true;
  }

  /* --------------------------------------------------------------------------
   */
  b8 reloadSymbolsFromObjFile(
    const ReloadContext& context, 
    ReloadResult*        result)
  {
    String hrfpath = context.hrfpath;
    String exepath = context.exepath;

    if (!talloc.init())
      return ERROR("failed to init temp allocator\n");

    defer { talloc.deinit(); };

    // Handle swapping prev and curr patch.

    // TODO(sushi) do in init
    if (!patch.base.isValid())
      if (!patch.base.initBase(
            &mem::stl_allocator,
            context.reloadee_handle, 
            exepath))
        return ERROR("failed to initialize base patch\n");

    if (patch.prev.isValid())
      patch.prev.deinit();

    patch.curr.moveTo(&patch.prev);

    // Form the name of the expected patched executable and load it.

    io::StaticBuffer<512> patch_path_buf;
    io::formatv(&patch_path_buf, exepath, ".patch", reload_idx, ".so");

    reload_idx += 1;

    if (!patch.curr.init(
          &mem::stl_allocator,
          patch_path_buf.asStr()))
      return ERROR("failed to create Patch for '", patch_path_buf.asStr(), 
                   "'\n");

    // Form a list of filtered symbols.
    StringSet filtered_symbols;
    if (!filtered_symbols.init())
      return ERROR("failed to init filtered symbols string set\n");
    defer { filtered_symbols.deinit(); };

    if (!collectDefaultFilteredSymbols(&talloc, &filtered_symbols))
      return ERROR("failed to collect filtered symbols\n");

    // Form a list of symbols we want to patch.
    StringSet patchable_symbols;
    if (!patchable_symbols.init())
      return ERROR("failed to init patchable symbols string set\n");
    defer { patchable_symbols.deinit(); };

    if (!collectPatchableSymbols(
          &talloc, 
          &patchable_symbols, 
          &filtered_symbols,
          hrfpath))
      return ERROR("failed to collect patchable symbols\n");

    u64 remappings_written = 0;

    Patch* prev_patch = nullptr;
    if (patch.prev.isValid())
      prev_patch = &patch.prev;
    else
      prev_patch = &patch.base;

    if (!prev_patch->copyGlobalState(patch.curr, patchable_symbols))
        return ERROR("failed to copy global state from prev to curr patch\n");

    if (!patch.base.redirectFunctionsTo(patch.curr, patchable_symbols))
      return ERROR("failed to redirect function from base to curr patch\n");

    result->remappings_written = remappings_written;

    return true;
  }

  /* --------------------------------------------------------------------------
   */
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
Reloader* createReloader()
{
  auto* r = mem::stl_allocator.construct<Reloader>();
  r->init();
  return r;
}

/* ----------------------------------------------------------------------------
 */
b8 doReload(
    Reloader*  r,
    const ReloadContext& context,
    ReloadResult* result)
{
  return r->reloadSymbolsFromObjFile(context, result);
}

/* ----------------------------------------------------------------------------
 */
u64 getPatchNumber(Reloader* r)
{
  return r->reload_idx;
}

}
