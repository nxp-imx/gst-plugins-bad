wl_sources = [
    'gstwaylandsink.c',
    'wlshmallocator.c',
    'wlbuffer.c',
    'wldisplay.c',
    'wlwindow.c',
    'wlvideoformat.c',
    'wllinuxdmabuf.c',
    'wlutils.c',
]

libdrm_dep = dependency('libdrm', version: '>= 2.4.55', required:get_option('wayland'))

ionallocator_flags = []

if cc.has_header('linux/ion.h') and cc.has_header('gst/allocators/gstionmemory.h', dependencies: gstallocators_dep)
    ionallocator_flags += ['-DHAVE_ION_ALLOCATOR']
endif

dmabufheapsallocator_flags = []

if cc.has_header('linux/dma-heap.h') and cc.has_header('gst/allocators/gstdmabufheaps.h', dependencies: gstallocators_dep)
    dmabufheapsallocator_flags += ['-DHAVE_DMABUFHEAPS_ALLOCATOR']
endif

if use_wayland
    protocols_datadir = wl_protocol_dep.get_variable('pkgdatadir')

    protocol_defs = [
        ['/stable/viewporter/viewporter.xml', 'viewporter-protocol.c', 'viewporter-client-protocol.h'],
        ['/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml',
         'linux-dmabuf-unstable-v1-protocol.c', 'linux-dmabuf-unstable-v1-client-protocol.h'],
        ['/unstable/fullscreen-shell/fullscreen-shell-unstable-v1.xml',
         'fullscreen-shell-unstable-v1-protocol.c', 'fullscreen-shell-unstable-v1-client-protocol.h'],
        ['/stable/xdg-shell/xdg-shell.xml', 'xdg-shell-protocol.c', 'xdg-shell-client-protocol.h'],
        ['/unstable/alpha-compositing/alpha-compositing-unstable-v1.xml',
         'alpha-compositing-unstable-v1-protocol.c', 'alpha-compositing-unstable-v1-client-protocol.h'],
        ['/unstable/linux-explicit-synchronization/linux-explicit-synchronization-unstable-v1.xml',
         'linux-explicit-synchronization-unstable-v1-protocol.c', 'linux-explicit-synchronization-unstable-v1-client-protocol.h'],
        ['/unstable/hdr10-metadata/hdr10-metadata-unstable-v1.xml',
         'hdr10-metadata-unstable-v1-protocol.c', 'hdr10-metadata-unstable-v1-client-protocol.h'],
    ]
    protocols_files = []

    foreach protodef: protocol_defs
        xmlfile = protocols_datadir + protodef.get(0)

        protocols_files += [custom_target(protodef.get(1),
          output : protodef.get(1),
          input : xmlfile,
          command : [wl_scanner, 'code', '@INPUT@', '@OUTPUT@'])]

        protocols_files += [custom_target(protodef.get(2),
          output : protodef.get(2),
          input : xmlfile,
          command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])]
    endforeach

    gstwaylandsink = library('gstwaylandsink',
        wl_sources + protocols_files,
        c_args : gst_plugins_bad_args + ionallocator_flags + dmabufheapsallocator_flags + ['-DGST_USE_UNSTABLE_API'],
        include_directories : [configinc],
        dependencies : [gst_dep, gstvideo_dep, gstwayland_dep, gstallocators_dep,
                        wl_client_dep, wl_protocol_dep, libdrm_dep],
        install : true,
        install_dir : plugins_install_dir,
    )
    pkgconfig.generate(gstwaylandsink, install_dir : plugins_pkgconfig_install_dir)
    plugins += [gstwaylandsink]
endif
