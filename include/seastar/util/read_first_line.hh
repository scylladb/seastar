#include <filesystem>
#include <seastar/core/sstring.hh>
#ifndef SEASTAR_MODULE
#include <boost/lexical_cast.hpp>
#endif

namespace seastar {

sstring read_first_line(std::filesystem::path sys_file);

template <typename Type>
Type read_first_line_as(std::filesystem::path sys_file) {
    return boost::lexical_cast<Type>(read_first_line(sys_file));
}

}
