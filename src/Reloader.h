/*
 *  Type representing the actual logic used to perform reloading.
 *
 *  TODO(sushi) the way this is organized isn't really necessary anymore. I 
 *              initially thought I needed to hide the reloader's symbols 
 *              to prevent them getting patched during a reload, so I made it
 *              so that the interface is loaded dynamically.
 */

#ifndef _hreload_reloader_h
#define _hreload_reloader_h

#include "iro/common.h"
#include "iro/unicode.h"
#include "iro/containers/slice.h"

using namespace iro;

namespace hr
{

struct Reloader;

struct Remapping
{
  String symbol;

  enum class Kind
  {
    // Memory that should be copied from the old to the new addr.
    Mem,
    // A function that needs to be patched with some code to redirect it 
    // to the new function call.
    Func,
  };

  Kind kind;

  union
  {
    struct
    {
      // The size of the weak global thing.
      u64 size;
    } mem;

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
  // Path to the .hrf file generated for the executable being reloaded. This
  // is just a newline delimited list of all the object files used 
  String hrfpath;

  // Path to the patched or initial executable of the reloaded process. 
  // The patched executable is required to have been generated with a name of 
  // the format:
  //   <name>.patch<n>.so
  // where <n> is the patch number as reported by the ReloaderPatchNumberFunc 
  // function.
  // TODO(sushi) I don't think that keeping these patches around is necessary
  //             anymore.
  String exepath;

  // Handle to the dynamic symbol table of the thing being reloaded.
  // Eg. this comes from dlopen(nullptr, ...).
  void* reloadee_handle;
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

Reloader* createReloader();

b8 doReload(
    Reloader* r, 
    const ReloadContext& context, 
    ReloadResult* out_result);

u64 getPatchNumber(Reloader* r);

}

#endif
