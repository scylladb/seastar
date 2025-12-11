#define BOOST_TEST_MODULE core

#include <seastar/util/variant_utils.hh>

#include <boost/test/unit_test.hpp>

#include <variant>

struct noncopyable_type {
    noncopyable_type() = default;
    ~noncopyable_type() = default;
    noncopyable_type(const noncopyable_type&) = delete;
    noncopyable_type& operator=(const noncopyable_type&) = delete;
    noncopyable_type(noncopyable_type&&) noexcept = default;
    noncopyable_type& operator=(noncopyable_type&&) noexcept = default;
};

using variant_t = std::variant<int, bool>;
static noncopyable_type t{};

static_assert(
  std::is_same_v<
    decltype(seastar::visit(
      variant_t{true},
      [](const int& i) -> noncopyable_type { return noncopyable_type{}; },
      [](const bool& b) -> noncopyable_type { return noncopyable_type{}; })),
    noncopyable_type>);

static_assert(std::is_same_v<
              decltype(seastar::visit(
                variant_t{true},
                [](const int& i) -> noncopyable_type& { return t; },
                [](const bool& b) -> noncopyable_type& { return t; })),
              noncopyable_type&>);

static_assert(
  std::is_same_v<
    decltype(std::visit(
      [](auto&& v) -> noncopyable_type& { return t; }, variant_t{true})),
    decltype(seastar::visit(
      variant_t{true},
      [](const int& i) -> noncopyable_type& { return t; },
      [](const bool& b) -> noncopyable_type& { return t; }))>);

static_assert(
  std::is_same_v<
    decltype(std::visit(
      [](auto&& v) -> noncopyable_type { return noncopyable_type{}; },
      variant_t{true})),
    decltype(seastar::visit(
      variant_t{true},
      [](const int& i) -> noncopyable_type { return noncopyable_type{}; },
      [](const bool& b) -> noncopyable_type { return noncopyable_type{}; }))>);

BOOST_AUTO_TEST_CASE(test_visit_can_return_reference) {
    auto& std_visit_ref = std::visit(
      [](auto&& v) -> noncopyable_type& { return t; }, variant_t{true});
    auto& ss_visit_ref = seastar::visit(
      variant_t{true},
      [](const int& i) -> noncopyable_type& { return t; },
      [](const bool& b) -> noncopyable_type& { return t; });
    BOOST_REQUIRE_EQUAL(
      std::addressof(std_visit_ref), std::addressof(ss_visit_ref));
    BOOST_REQUIRE_EQUAL(std::addressof(t), std::addressof(std_visit_ref));
}
