#include "stdio.h"
#include "dlfcn.h"
#include "sys/mman.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"
#include "link.h"
#include "stdlib.h"

#include "iro/common.h"
#include "iro/fs/file.h"
#include "iro/logger.h"
#include "iro/process.h"
#include "iro/platform.h"

#include "Reloader.h"

#define DEFINE_GDB_PY_SCRIPT(script_name) \
  asm("\
.pushsection \".debug_gdb_scripts\", \"MS\",@progbits,1\n\
.byte 1 /* Python */\n\
.asciz \"" script_name "\"\n\
.popsection \n\
");

DEFINE_GDB_PY_SCRIPT("lpp-gdb.py");

using namespace iro;

inline Logger logger = Logger::create("hreload"_str, Logger::Verbosity::Info);

static void orange()
{
  INFO("orange\n");
}

static void banana(int a, int b, int c, int d)
{
  INFO("a+b+c+d= ", a+b+c+d, "\n");
  orange();
}

extern "C" void apple()
{
  int a = 0;
  int b = 1;
  INFO("new apple\n");
  banana(1, 2, 3, 4);
}

extern "C" void syncGlobal(void* dest, void* src, u64 size, b8 to_me)
{
  if (to_me)
  {
    void* aligned = (void*)((size_t)dest & -getpagesize());
    if (mprotect(aligned, getpagesize(), PROT_READ | PROT_WRITE))
    {
      printf("err in mprotect: %s\n", strerror(errno));
    }
  }
  memcpy(dest, src, size);
}

typedef void (*SyncGlobalFunc)(void*, void*, u64, b8);

b8 doReload(
    Reloader* r, 
    ReloadFunc reloader, 
    ReloaderPatchNumberFunc getPatchNumber,
    void* hrlib)
{
  fs::stdout.write("press enter to reload...\n"_str);

  u8 dummy[255];
  fs::stdin.read(Bytes::from(dummy, 255));

  io::StaticBuffer<32> patchnumbuf;
  patchnumbuf.open();
  io::format(&patchnumbuf, getPatchNumber(r));

  str args[2] =
  {
    "patch"_str,
    patchnumbuf.asStr(),
  };

  Process::Stream streams[3] =
  {
    { false, nullptr },
    { false, nullptr },
    { false, nullptr },
  };

  auto lake = 
    Process::spawn(
      "/home/sushi/src/enosi/bin/lake"_str,
      Slice<str>::from(args, 2),
      streams,
      "/home/sushi/src/enosi"_str);

  while (lake.status == Process::Status::Running)
    lake.checkStatus();

  void* dlhandle = dlopen(nullptr, RTLD_LAZY);

  const u64 num_remappings = 1028;
  const u64 mapsize = sizeof(Remapping) * num_remappings;

  auto* remappings = 
    (Remapping*)mmap(
      0, 
      mapsize, 
      PROT_WRITE | PROT_READ | PROT_EXEC, 
      MAP_SHARED | MAP_ANONYMOUS, 
      -1, 
      0);

  if (remappings == MAP_FAILED)
    return ERROR("failed to create mapped data for reloader: ",
                 strerror(errno), "\n");

  defer { munmap(remappings, mapsize); };

  ReloadContext context;
  context.odefpath = "build/debug/hreload.odef"_str;
  context.exepath = "build/debug/hreload"_str;
  context.reloadee_handle = dlhandle;
  context.remappings = { remappings, num_remappings };

  ReloadResult result;

  if (!reloader(r, context, &result))
    return ERROR("failed to reload symbols\n");

  for (u64 remapping_idx = 0; 
       remapping_idx < result.remappings_written; 
       ++remapping_idx)
  {
    Remapping* remapping = remappings + remapping_idx;

    // Don't remap this function.
    if (remapping->old_addr == &doReload)
      continue;

    TRACE("remapping ", remapping->old_addr, " to ", remapping->new_addr, 
          "\n");

    switch (remapping->kind)
    {
    case Remapping::Kind::Func:
      {
        void* aligned = (void*)((size_t)remapping->old_addr & -getpagesize());
        if (mprotect(aligned, 4096, PROT_EXEC | PROT_READ | PROT_WRITE))
          return ERROR("failed to mprotect ", aligned, ": ", 
                       strerror(errno), "\n");
        memcpy(remapping->old_addr, remapping->func.bytes, 16);
      }
      break;

    case Remapping::Kind::Mem:
      {
        void* aligned = (void*)((size_t)remapping->new_addr & -getpagesize());
        if (mprotect(aligned, 4096*2, PROT_EXEC | PROT_READ | PROT_WRITE))
          return ERROR("failed to mprotect ", aligned, ": ", 
                       strerror(errno), "\n");
        memcpy(remapping->new_addr, remapping->old_addr, remapping->mem.size);
      }
      break;
    }
  }

  return true;
}

int main()
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

  INFO("opening lib\n");
  void* hrlib = dlopen("build/debug/libhreloader.so", RTLD_LAZY);
  INFO("lib opened\n");

  struct link_map* lm;
  dlinfo(dlopen(nullptr, RTLD_LAZY), RTLD_DI_LINKMAP, &lm);

  INFO("addr: ", (void*)lm->l_addr, "\n");

  auto create_reloader =
    (Reloader* (*)())dlsym(hrlib, "hreloadCreateReloader");

  auto reload = 
    (ReloadFunc)dlsym(
      hrlib, 
      "hreloadReloadSymbolsFromObjFile");

  auto getPatchNumber = 
    (ReloaderPatchNumberFunc)dlsym(
      hrlib,
      "hreloadPatchNumber");

  if (!create_reloader)
    return ERROR("failed to load reloader: ", dlerror(), "\n");

  void* dlhandle = dlopen(nullptr, RTLD_LAZY);

  INFO("dlhandle: ", dlhandle, "\n");

  Reloader* reloader = create_reloader();

  void* inthandle = nullptr;

  auto test = (void (*)(Array<u32>*))dlsym(hrlib, "test");

  auto copyLog = (void (*)(iro::Log*))dlsym(hrlib, "copyLog");

  copyLog(&iro::log);

  Array<u32> arr;
  arr.init();

  test(&arr);

  arr.clear();

  for (;;)
  {
    doReload(reloader, reload, getPatchNumber, hrlib);
    apple();
  }
}
