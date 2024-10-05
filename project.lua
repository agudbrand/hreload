local enosi = require "enosi"
local recipes = require "recipes"
local Driver = require "driver"
local proj = enosi.thisProject()
local driver = require "driver"

local os = lake.os()
local cwd = lake.cwd()

local mode = enosi.mode
local builddir = cwd.."/build/"..mode.."/"

proj:setCleaner(function()
  lake.rm(builddir, {recursive=true,force=true})
end)

local hreload = lake.target(builddir.."hreload")

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

if os == "Linux" then
  proj:reportDefine { "HRELOAD_LINUX", "1" }
elseif os == "Windows" then
  proj:reportDefine { "HRELOAD_WIN32", "1" }
else
  error("unhandled OS for hreload")
end

cpp_driver.defines = proj:collectDefines()

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

hreload:dependsOn(proj:collectObjFiles())

local link_driver = Driver.Linker.new()

local inputs = proj:collectObjFiles()

link_driver.static_libs = proj:collectStaticLibs()
link_driver.shared_libs = proj:collectSharedLibs()
link_driver.inputs = inputs
link_driver.libdirs = proj:collectLibDirs()
link_driver.output = hreload.path

hreload:recipe(recipes.linker(link_driver, proj))
