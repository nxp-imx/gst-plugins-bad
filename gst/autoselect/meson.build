autocon_sources = [
  'gstautoselect.c',
  'gstautovideoconvert2.c',
  'plugin.c',
]

gstautoselect = library('gstautoselect',
  autocon_sources,
  c_args : gst_plugins_bad_args,
  include_directories : [configinc],
  dependencies : [gstbase_dep],
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gstautoselect]
