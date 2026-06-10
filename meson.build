project('beamspot', 'cpp',
  version: '1.0',
  default_options: ['cpp_std=c++17', 'warning_level=2', 'buildtype=release'])

fs = import('fs')
cc = meson.get_compiler('cpp')

# --- ROOT via root-config -------------------------------------------------
# ROOT does not ship a reliable pkg-config file across installs, so drive it
# through root-config. The project pins cpp_std=c++17 above to match the
# standard ROOT 6.38 was built with (`root-config --cflags`); align cpp_std
# if your ROOT reports a different standard.
root_config = find_program('root-config')
root_cflags = run_command(root_config, '--cflags', check: true).stdout().strip().split()
root_libs   = run_command(root_config, '--libs',   check: true).stdout().strip().split()

root_cflags_clean = []
foreach f : root_cflags
  if not f.startswith('-std=')  # meson owns the C++ standard
    root_cflags_clean += f
  endif
endforeach

root_dep = declare_dependency(
  compile_args: root_cflags_clean,
  link_args: root_libs)

# --- hipo4 (vendored under external/hipo4) --------------------------------
# hipo needs lz4 and fmt. lz4 is resolved via pkg-config. fmt is used
# header-only when its pkg-config/cmake config is not on the search path
# (e.g. homebrew's fmt under a non-default prefix).
# hipo gates LZ4 (de)compression behind -D__LZ4__; CLAS12 DSTs are LZ4-compressed,
# so the define is required, not optional.
lz4_base = dependency('liblz4', required: true)
lz4_dep = declare_dependency(dependencies: lz4_base, compile_args: ['-D__LZ4__'])

fmt_dep = dependency('fmt', required: false)
if not fmt_dep.found()
  foreach prefix : ['/opt/homebrew', '/usr/local', '/opt/local']
    if not fmt_dep.found() and fs.exists(prefix / 'include' / 'fmt' / 'format.h')
      message('using header-only fmt from ' + prefix)
      fmt_dep = declare_dependency(
        compile_args: ['-I' + prefix / 'include', '-DFMT_HEADER_ONLY'])
    endif
  endforeach
endif
assert(fmt_dep.found(), 'fmt not found: install fmt or add its prefix to meson.build')

project_libs = []
subdir('external/hipo4')   # defines hipo_dep (include + link) and hipo_lib

# --- beamspot -------------------------------------------------------------
inc = include_directories('include')

beamspot_lib = static_library('beamspot_core',
  'src/beam_spot.cxx',
  include_directories: inc,
  dependencies: [root_dep, hipo_dep])

executable('beamspot',
  'src/main.cxx',
  include_directories: inc,
  link_with: beamspot_lib,
  dependencies: [root_dep, hipo_dep],
  install: true)
