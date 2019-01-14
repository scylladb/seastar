#include <experimental/filesystem>
#include <seastar/core/sstring.hh>

namespace seastar {

sstring read_first_line(std::experimental::filesystem::path sys_file);

}
