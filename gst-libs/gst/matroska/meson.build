matroska_sources = [
  'gstmatroska.c',
]
matroska_headers = [
  'gstmatroska.h',
]
install_headers(matroska_headers, subdir : 'gstreamer-1.0/gst/matroska')

gstmatroska = library('gstmatroska-' + api_version,
  matroska_sources,
  c_args : gst_plugins_bad_args + ['-DGST_USE_UNSTABLE_API', '-DBUILDING_GST_MATROSKA', '-DG_LOG_DOMAIN="GStreamer-MATROSKA"'],
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  darwin_versions : osxversion,
  install : true,
  dependencies : [gstbase_dep],
)

gstmatroska_dep = declare_dependency(link_with : gstmatroska,
  include_directories : [libsinc],
  dependencies : [gstbase_dep])
