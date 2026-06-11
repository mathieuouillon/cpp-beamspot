hipo_sources = files(
  'bank.cpp',
  'datastream.cpp',
  'dictionary.cpp',
  'event.cpp',
  'fusion.cpp',
  'node.cpp',
  'parser.cpp',
  'reaction.cpp',
  'reader.cpp',
  'record.cpp',
  'recordbuilder.cpp',
  'tuple.cpp',
  'utils.cpp',
  'wrapper.cpp',
  'writer.cpp',
)
hipo_headers = files(
  'bank.h',
  'constants.h',
  'datastream.h',
  'dictionary.h',
  'event.h',
  'fizika.h',
  'fusion.h',
  'hipoexceptions.h',
  'json.h',
  'jsonutil.h',
  'node.h',
  'parser.h',
  'reaction.h',
  'reader.h',
  'record.h',
  'recordbuilder.h',
  'tuple.h',
  'twig.h',
  'utils.h',
  'writer.h',
  'hipoeventiterator.h',
  'threadpool.hpp',
  'progresstracker.hpp',
  'chain.h',
)

hipo_lib = library(
  'hipo4',
  hipo_sources,
  dependencies: [ lz4_dep, fmt_dep],
  override_options: ['warning_level=0'],  # vendored third-party: don't warn on it
  install: true
)

install_headers(hipo_headers, subdir: 'hipo4')
project_libs += hipo_lib

hipo_dep = declare_dependency(
  # `..` so consumers use `#include "hipo4/___.h"`; is_system so the vendored
  # headers are -isystem and their warnings don't surface in our build.
  include_directories: include_directories('..', is_system: true),
  link_with: hipo_lib,
  dependencies: [fmt_dep],
)
