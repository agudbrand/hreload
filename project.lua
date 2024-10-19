local enosi = require "enosi"
local recipes = require "recipes"
local Driver = require "driver"
local proj = enosi.thisProject()
local driver = require "driver"
local List = require "list"

local os = lake.os()
local cwd = lake.cwd()

local mode = enosi.mode
local builddir = cwd.."/build/"..mode.."/"

proj:setCleaner(function()
  lake.rm(builddir, {recursive=true,force=true})
end)

proj:dependsOn("iro")

local cpp_driver = driver.Cpp.new()

proj:reportIncludeDir(cwd.."/src")

cpp_driver.include_dirs:push { "src", proj:collectIncludeDirs() }

if mode == "debug" then
  cpp_driver.opt = "none"
  cpp_driver.debug_info = true
  proj:reportDefine { "HRELOAD_DEBUG", "1" }
else
  cpp_driver.opt = "speed"
end

cpp_driver.defines = proj:collectDefines()
cpp_driver.export_all = true

lake.find("src/**/*.cpp"):each(function(cfile)
  local ofile = builddir..cfile..".o"
  local dfile = builddir..cfile..".d"
  cfile = cwd.."/"..cfile

  cpp_driver.input = cfile
  cpp_driver.output = ofile

  proj:reportObjFile(ofile)

  lake.target(ofile)
      :dependsOn { cfile, proj.path }
      :recipe(recipes.objFile(cpp_driver, proj))

  lake.target(dfile)
      :dependsOn(cfile)
      :recipe(
        recipes.depfile(
          driver.Depfile.fromCpp(cpp_driver),
          proj, ofile, dfile))
end)

--- Build the test executable.

local test_ofiles = List{}

lake.find("tests/**/*.cpp"):each(function(cfile)
  local ofile = builddir..cfile..".o"
  local dfile = builddir..cfile..".d"
  cfile = cwd.."/"..cfile

  cpp_driver.input = cfile
  cpp_driver.output = ofile

  test_ofiles:push(ofile)

  lake.target(ofile)
      :dependsOn { cfile, proj.path }
      :recipe(recipes.objFile(cpp_driver, proj))

  lake.target(dfile)
      :dependsOn(cfile)
      :recipe(
        recipes.depfile(
          driver.Depfile.fromCpp(cpp_driver),
          proj, ofile, dfile))
end)

local test_link = Driver.Linker.new()
local testpath
if enosi.patch then
  testpath = builddir.."test.patch"..enosi.patch..".so"
  test_link.shared_lib = true
else
  testpath = builddir.."test"
end

local testexe = lake.target(testpath)

test_link.static_libs = proj:collectStaticLibs()
test_link.shared_libs = proj:collectSharedLibs()
test_link.libdirs = proj:collectLibDirs()
test_link.inputs = { test_ofiles,  proj:collectObjFiles() }
test_link.output = testexe.path

testexe
  :dependsOn(test_link.inputs)
  :recipe(recipes.linker(test_link, proj))

local testhrf = lake.target(builddir.."test.hrf")

testhrf
  :dependsOn(testexe.path)
  :recipe(function()
    local f = io.open(testhrf.path, "w")
    if not f then
      error("failed to open "..testhrf.path.." for writing")
    end

     -- Filter out luajit object files.
    -- TODO(sushi) get the absolute path. Need to setup projects storing their
    --             working directory.
    lake.find("../luajit/obj/*.o"):each(function(ofile)
      f:write("-o", ofile, "\n")
    end)

    --- Filter out explain stuff.
    f:write("-lexplain\n")

    --- Ensure that iro symbols are patched.
    enosi.getProject"iro".artifacts.obj_files:each(function(ofile)
      f:write("+o",ofile,"\n")
    end)
    
    test_ofiles:each(function(ofile)
      f:write("+o",ofile,"\n")
    end)

    proj.artifacts.obj_files:each(function(ofile)
      f:write("-o",ofile,"\n")
    end)
  end)  

-- local link_driver = Driver.Linker.new()
-- 
-- local inputs = proj:collectObjFiles()
-- 
-- local link_lib_driver = Driver.Linker.new()
-- 
-- link_lib_driver.static_libs = proj:collectStaticLibs()
-- link_lib_driver.shared_libs = proj:collectSharedLibs()
-- link_lib_driver.inputs = List(inputs)
-- link_lib_driver.libdirs = proj:collectLibDirs()
-- link_lib_driver.shared_lib = true
-- 
-- local hreloaderlib = lake.target(builddir.."libhreloader.so")
-- local hreloadero = lake.target(builddir.."src/Reloader.cpp.o")
-- 
-- cpp_driver.input = "src/Reloader.cpp"
-- cpp_driver.output = hreloadero.path
-- hreloadero
--   :dependsOn {"src/Reloader.cpp", enosi.getProject"iro":collectObjFiles() }
--   :recipe(recipes.objFile(cpp_driver, proj))
-- 
-- link_lib_driver.inputs:push(hreloadero.path)
-- link_lib_driver.output = hreloaderlib.path
-- hreloaderlib
--   :dependsOn(hreloadero.path)
--   :recipe(recipes.linker(link_lib_driver, proj))
-- 
-- link_driver.static_libs = proj:collectStaticLibs()
-- link_driver.shared_libs = proj:collectSharedLibs()
-- link_driver.inputs = inputs
-- link_driver.libdirs = proj:collectLibDirs()
-- 
-- link_driver.shared_libs:push("hreloader")
-- link_driver.libdirs:push(builddir)
-- 
-- local hreload = lake.target(builddir.."hreload")
-- if enosi.patch then
--   hreload = lake.target(builddir.."hreload.patch"..enosi.patch..".so")
--   link_driver.shared_lib = true
-- else
--   hreload = lake.target(builddir.."hreload")
-- end
-- 
-- link_driver.output = hreload.path
-- 
-- hreload:dependsOn {proj:collectObjFiles(), hreloaderlib.path}
-- 
-- hreload:recipe(recipes.linker(link_driver, proj))
-- 
-- --- Output a .hrf (hot reload filter) file specifying object files and libs 
-- --- containing symbols we want to reload and ones we dont.
-- local odef = lake.target(hreload.path..".hrf")
-- odef:dependsOn(hreload.path)
-- odef:recipe(function()
--   local f = io.open(odef.path, "w")
--   if not f then
--     error("failed to open '"..odef.path.."' for writing")
--   end
-- 
--   -- Filter out luajit object files.
--   -- TODO(sushi) get the absolute path. Need to setup projects storing their
--   --             working directory.
--   lake.find("../luajit/obj/*.o"):each(function(ofile)
--     f:write("-o", ofile, "\n")
--   end)
-- 
--   --- Filter out explain stuff.
--   f:write("-lexplain\n")
-- 
--   lake.flatten(proj:collectObjFiles()):each(function(ofile)
--     f:write("+o", ofile, "\n")
--   end)
-- 
--   -- Don't reload the reloaders functions.
--   -- NOTE(sushi) this is only going to filter out symbols that don't appear in 
--   --             previous object files already, eg. it should only filter out
--   --             the hreload symbols and not the iro symbols that were already
--   --             marked as patchable above.
--   -- TODO(sushi) this should ideally be done internally, but for some reason 
--   --             I can't get the lib's path from dlinfo, so I don't have any
--   --             good way to tell it where it is. 
--   f:write("-l", hreloaderlib.path, "\n")
--   f:close()
-- end)




