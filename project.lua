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

proj:reportIncludeDir(cwd)

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
  if cfile:find("Reloader") then
    return
  end
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

local link_driver = Driver.Linker.new()

local inputs = proj:collectObjFiles()

link_driver.static_libs = proj:collectStaticLibs()
link_driver.shared_libs = proj:collectSharedLibs()
link_driver.inputs = inputs
link_driver.libdirs = proj:collectLibDirs()

local hreload = lake.target(builddir.."hreload")
if enosi.patch then
  hreload = lake.target(builddir.."hreload.patch"..enosi.patch..".so")
  link_driver.shared_lib = true
else
  hreload = lake.target(builddir.."hreload")
end

link_driver.output = hreload.path

hreload:dependsOn(proj:collectObjFiles())

hreload:recipe(recipes.linker(link_driver, proj))

local link_lib_driver = Driver.Linker.new()

link_lib_driver.static_libs = proj:collectStaticLibs()
link_lib_driver.shared_libs = proj:collectSharedLibs()
link_lib_driver.inputs = List(inputs)
link_lib_driver.libdirs = proj:collectLibDirs()
link_lib_driver.shared_lib = true

local hreloaderlib = lake.target(builddir.."libhreloader.so")
local hreloadero = lake.target(builddir.."src/Reloader.cpp.o")

cpp_driver.input = "src/Reloader.cpp"
cpp_driver.output = hreloadero.path
hreloadero
  :dependsOn("src/Reloader.cpp")
  :recipe(recipes.objFile(cpp_driver, proj))

link_lib_driver.inputs:push(hreloadero.path)
link_lib_driver.output = hreloaderlib.path
hreloaderlib
  :dependsOn(hreloadero.path)
  :recipe(recipes.linker(link_lib_driver, proj))





