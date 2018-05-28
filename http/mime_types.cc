//
// mime_types.cpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include "mime_types.hh"

namespace seastar {

namespace httpd {
namespace mime_types {

struct mapping {
    const char* extension;
    const char* mime_type;
} mappings[] = {
        { "json", "application/json"},
        { "gif", "image/gif" },
        { "htm", "text/html" },
        { "css", "text/css" },
        { "js", "text/javascript" },
        { "html", "text/html" },
        { "jpg", "image/jpeg" },
        { "png", "image/png" },
        { "txt", "text/plain" },
        { "ico", "image/x-icon" },
        { "bin", "application/octet-stream" },
        { "proto", "application/vnd.google.protobuf; proto=io.prometheus.client.MetricFamily; encoding=delimited"},
};

const char* extension_to_type(const sstring& extension)
{
    for (mapping m : mappings) {
        if (extension == m.extension) {
            return m.mime_type;
        }
    }
    return "text/plain";
}

} // namespace mime_types

} // httpd

}
