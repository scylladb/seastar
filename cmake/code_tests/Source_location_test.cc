#if __has_include(<source_location>)
#include <source_location>
#endif

#ifdef __cpp_lib_source_location
using source_location = std::source_location;
#elif __has_include(<experimental/source_location>)
#include <experimental/source_location>
using source_location = std::experimental::source_location;
#endif

#if defined(__cpp_lib_source_location) || defined(__cpp_lib_experimental_source_location)
struct format_info {
    format_info(source_location loc = source_location::current()) noexcept
        : loc(loc)
    { }
    source_location loc;
};
#else
struct format_info { };
#endif

int main()
{
    format_info fi;
}
