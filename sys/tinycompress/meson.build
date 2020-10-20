if get_option('tinycompress').disabled()
  subdir_done()
endif

have_tinycompress = cc.has_header ('tinycompress/tinycompress.h')
if have_tinycompress
  tinycompress_dep = cc.find_library('tinycompress', required: false)
  have_tinycompress = tinycompress_dep.found()
endif

if not have_tinycompress
  tinycompress_dep = dependency('tinycompress', required: false,
                            fallback: ['tinycompress', 'tinycompress_dep'])
  have_tinycompress = tinycompress_dep.found()
endif

if have_tinycompress
  gsttinycompress = library('gsttinycompress',
    'tinycompress.c', 'tinycompresssink.c',
    c_args: gst_plugins_bad_args,
    include_directories: [configinc],
    dependencies : [gstbase_dep, gstaudio_dep, tinycompress_dep],
    install: true,
    install_dir: plugins_install_dir
  )
  pkgconfig.generate(gsttinycompress, install_dir: plugins_pkgconfig_install_dir)
elif get_option('tinycompress').enabled()
  message('tinycompress plugin enabled but TinyALSA library or headers not found')
  subdir_done()
endif
