/*
 *  Type representing the actual logic used to perform reloading.
 */

#ifndef _hreload_reloader_h
#define _hreload_reloader_h

#include "iro/common.h"
#include "iro/unicode.h"
#include "iro/containers/slice.h"

using namespace iro;

struct Reloader;

struct Remapping
{
  enum class Kind
  {
    Weak,
    Func,
  };

  Kind kind;

  union
  {
    struct
    {
      // The size of the weak global thing.
      u64 size;
    } weak;

    struct 
    {
      // The bytes to inject at the beginning of the function.
      u8 bytes[16];
    } func;
  };

  // The address of the old thing.
  void* old_addr;

  // The address of the new thing;
  void* new_addr;

};

struct ReloadContext
{
  // Path to the object file that we will search for symbols to reload.
  str objpath;

  // Path to the normal executable of the reloaded process. The patched
  // executable is required to have been generated with a name of the 
  // format:
  //   <name>.patch<n>.so
  // where <n> is the patch number as reported by the 
  // ReloaderPatchNumberFunc function.
  str exepath;

  // Handle to the dynamic symbol table of the thing being reloaded.
  // Eg. this comes from dlopen(nullptr, ...).
  void* reloadee_handle;

  // Mapped array of function remappings that the reloadee is expected
  // to write after reloading is finished. This must have been mapped 
  // using mmap with the MAP_SHARED flag.
  Slice<Remapping> remappings;
};

struct ReloadResult
{
  u64 remappings_written;

  void* this_patch;
  void* prev_patch;
};

// The type of the function that creates and initializes the reloader.
// Passed to the ReloadFunc.
typedef Reloader* (*CreateReloaderFunc)();

// The type of the symbol reloading function to be dynamically loaded.
typedef b8 (*ReloadFunc)(Reloader*,const ReloadContext&, ReloadResult*);

// The type of the reloader's patch number reporter function.
typedef u64 (*ReloaderPatchNumberFunc)(Reloader*);

#endif
