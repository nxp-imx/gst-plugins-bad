wl_sources = [
    'gstwaylandsink.c'
]

if use_wayland

    gstwaylandsink = library('gstwaylandsink',
        wl_sources + protocols_files,
        c_args : gst_plugins_bad_args + ['-DGST_USE_UNSTABLE_API'],
        include_directories : [configinc],
        dependencies : [gst_dep, gstvideo_dep, gstwayland_dep,
                        wl_client_dep, wl_protocol_dep, libdrm_dep],
        install : true,
        install_dir : plugins_install_dir,
    )
    plugins += [gstwaylandsink]
endif
