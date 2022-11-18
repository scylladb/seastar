//
// mime_types.hpp
// ~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#pragma once

#include <seastar/core/sstring.hh>

namespace seastar {

namespace http {

namespace mime_types {

/**
 * Convert a file extension into a MIME type.
 *
 * @param extension the file extension
 * @return the mime type as a string
 */
const char* extension_to_type(const sstring& extension);

} // namespace mime_types

} // namespace httpd

namespace httpd {
namespace mime_types {
[[deprecated("Use http::mime_types::extension_to_type instead")]]
inline const char* extension_to_type(const sstring& extension) {
    return http::mime_types::extension_to_type(extension);
}
}
}

}
