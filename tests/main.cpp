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
#include "iro/luastate.h"

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

void apple(int a, int b, int c, int d)
{
  INFO("a+b=", a+b+c+d, "\n");
}

void someLua(LuaState& lua, int a, int b, int c)
{
  lua.dostring("count = count + 400 print(count)");
  apple(1,2,a,c);
}

b8 doReload(hr::Reloader* r)
{
  fs::stdout.write("press enter to reload...\n"_str);

  u8 dummy[255];
  fs::stdin.read(Bytes::from(dummy, 255));

  io::StaticBuffer<32> patchnumbuf;
  patchnumbuf.open(); 
  io::format(&patchnumbuf, hr::getPatchNumber(r)); 

  String args[2] =
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
      Slice<String>::from(args, 2),
      streams,
      "/home/sushi/src/enosi"_str);

  while (lake.status == Process::Status::Running)
    lake.checkStatus();

  void* dlhandle = dlopen(nullptr, RTLD_LAZY);

  hr::ReloadContext context;
  context.hrfpath = "build/debug/test.hrf"_str;
  context.exepath = "build/debug/test"_str;
  context.reloadee_handle = dlhandle;

  hr::ReloadResult result;

  if (!hr::doReload(r, context, &result))
    return ERROR("failed to reload symbols\n");

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

  auto* reloader = hr::createReloader();

  LuaState lua;

  lua.init();

  lua.dostring(R"lua(
    count = 0
  )lua");

  for (;;)
  {
    doReload(reloader);
    someLua(lua, 5, 6, 7);
  }
}
