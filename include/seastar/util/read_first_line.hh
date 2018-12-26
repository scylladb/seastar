#include <seastar/util/std-compat.hh>
#include <seastar/core/sstring.hh>

namespace seastar {

sstring read_first_line(compat::filesystem::path sys_file);

}
