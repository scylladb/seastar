#pragma once

#ifdef SEASTAR_BUILD_SHARED_LIBS
#define SEASTAR_GLOBAL_DYNAMIC_TLS __attribute__((tls_model("global-dynamic")))
#else
#define SEASTAR_GLOBAL_DYNAMIC_TLS
#endif
