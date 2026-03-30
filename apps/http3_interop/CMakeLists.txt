seastar_add_app (http3_interop
  SOURCES
    main.cc)

target_link_libraries (app_http3_interop
  PRIVATE
    nghttp3::nghttp3)
