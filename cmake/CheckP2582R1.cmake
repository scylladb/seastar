include (CheckCXXSourceCompiles)
include (CMakePushCheckState)

cmake_push_check_state (RESET)

set (CMAKE_REQUIRED_FLAGS "-std=c++23")

# check if the compiler implements the inherited vs non-inherited guide
# tiebreaker specified by P2582R1, see https://wg21.link/P2582R1
check_cxx_source_compiles ("
template <typename T> struct B {
    B(T) {}
};

template <typename T> struct C : public B<T> {
    using B<T>::B;
};

B(int) -> B<char>;
C c2(42);

int main() {}
"
  Cxx_Compiler_IMPLEMENTS_P2581R1)

cmake_pop_check_state ()
