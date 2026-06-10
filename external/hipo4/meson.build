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
  install: true
)

install_headers(hipo_headers, subdir: 'hipo4')
project_libs += hipo_lib

hipo_dep = declare_dependency(
  include_directories: include_directories('..'), # use `..` so consumers use `#include "hipo4/___.h"`
  link_with: hipo_lib,
  dependencies: [fmt_dep],
)
