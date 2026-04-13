cmocka_dep = dependency('cmocka', required : true)

test_sources = {
  'test_crc32'     : files('test_crc32.c'),
  'test_bits'      : files('test_bits.c'),
  'test_parse'     : files('test_parse.c'),
  'test_pbi'       : files('test_pbi.c'),
  'test_uboot'     : files('test_uboot.c'),
  'test_binary'    : files('test_binary.c'),
  'test_decompile' : files('test_decompile.c'),
}

foreach name, src : test_sources
  exe = executable(name, src,
    dependencies : [libqoriq_rcw_dep, cmocka_dep],
    include_directories : src_inc)
  test(name, exe)
endforeach
