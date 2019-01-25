#include <seastar/core/posix.hh>
#include <seastar/util/read_first_line.hh>

namespace seastar {

sstring read_first_line(compat::filesystem::path sys_file) {
    auto file = file_desc::open(sys_file.string(), O_RDONLY | O_CLOEXEC);
    sstring buf;
    size_t n = 0;
    do {
        // try to avoid allocations
        sstring tmp(sstring::initialized_later{}, 8);
        auto ret = file.read(tmp.data(), 8ul);
        if (!ret) { // EAGAIN
            continue;
        }
        n = *ret;
        if (n > 0) {
            buf += tmp;
        }
    } while (n != 0);
    auto end = buf.find('\n');
    auto value = buf.substr(0, end);
    file.close();
    return value;
}

}
