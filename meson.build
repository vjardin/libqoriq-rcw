project('libqoriq-rcw', 'c',
  version : '0.1.0',
  license : 'BSD-3-Clause',
  default_options : [
    'c_std=c17',
    'warning_level=3',
    'default_library=both',
  ])

inc = include_directories('include')
src_inc = include_directories('src')

lib_sources = files(
  'src/rcw_context.c',
  'src/rcw_crc32.c',
  'src/rcw_bits.c',
  'src/rcw_parse.c',
  'src/rcw_pbi.c',
  'src/rcw_uboot.c',
  'src/rcw_binary.c',
  'src/rcw_decompile.c',
  'src/rcw_preprocess.c',
)

libqoriq_rcw = library('qoriq-rcw', lib_sources,
  include_directories : [inc, src_inc],
  install : true,
  version : meson.project_version())

libqoriq_rcw_dep = declare_dependency(
  link_with : libqoriq_rcw,
  include_directories : inc)

install_headers('include/qoriq-rcw/rcw.h',
  subdir : 'qoriq-rcw')

pkg = import('pkgconfig')
pkg.generate(libqoriq_rcw,
  name : 'libqoriq-rcw',
  description : 'QorIQ Reset Configuration Word compiler library (RCW)')

pandoc = find_program('pandoc', required : true)

man3 = custom_target('libqoriq-rcw.3',
  input : 'man/libqoriq-rcw.3.md',
  output : 'libqoriq-rcw.3',
  command : [pandoc, '-s', '-t', 'man', '@INPUT@', '-o', '@OUTPUT@'],
  install : true,
  install_dir : get_option('mandir') / 'man3')

subdir('tests')
