cmocka_dep = dependency('cmocka', required : true)

test_sources = {
  'test_crc32'     : files('test_crc32.c'),
  'test_bits'      : files('test_bits.c'),
  'test_parse'     : files('test_parse.c'),
  'test_pbi'       : files('test_pbi.c'),
  'test_uboot'     : files('test_uboot.c'),
  'test_binary'    : files('test_binary.c'),
  'test_decompile' : files('test_decompile.c'),
  'test_cond'      : files('test_cond.c'),
  'test_density'   : files('test_density.c'),
  'test_secure'    : files('test_secure.c'),
  'test_decode'    : files('test_decode.c'),
  'test_pbl'       : files('test_pbl.c'),
  'test_preprocess': files('test_preprocess.c'),
  'test_source'    : files('test_source.c'),
  'test_context'   : files('test_context.c'),
}

# The suites call internal functions, which the shared library does not export.
libqoriq_rcw_internal = static_library('qoriq-rcw-internal', lib_sources,
  include_directories : [inc, src_inc],
  dependencies : [mcpp_dep])

foreach name, src : test_sources
  exe = executable(name, src,
    link_with : libqoriq_rcw_internal,
    dependencies : [mcpp_dep, cmocka_dep],
    include_directories : [inc, src_inc])
  test(name, exe)
endforeach

test('check_exports', find_program('check_exports.sh'),
  args : [meson.project_source_root() / 'include' / 'qoriq-rcw' / 'rcw.h', mapfile])
