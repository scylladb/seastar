extern "C" {
#include <dlfcn.h>
}

int main() {
    dlopen("foobar", 42);
}
