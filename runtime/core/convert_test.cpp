// convert_test.cpp — Runtime-level tests for scalar-to-string conversion hooks.
//
// These tests verify that each conversion hook returns a fresh buffer
// owned by the current domain rather than a pointer to a shared static
// buffer.  Returning a static buffer silently corrupted any data
// structure (e.g. HashMap keys) that stored the returned string:
// the next conversion call overwrote the previous contents in place.
//
// Authority: docs/contracts/CONTRACT_RUNTIME_ABI.md §6

#include "dao_abi.h"

#include <boost/ut.hpp>
#include <cstdint>
#include <cstring>

using namespace boost::ut;

// NOLINTBEGIN(readability-magic-numbers)

suite<"conv_to_string_fresh"> conv_to_string_fresh = [] {
  // Core regression: two successive conversions must return distinct
  // pointers, so the caller can keep the first string alive while
  // computing the second.
  "i64_to_string returns distinct buffers"_test = [] {
    auto s1 = __dao_conv_i64_to_string(0);
    auto s2 = __dao_conv_i64_to_string(1);
    expect(s1.ptr != s2.ptr) << "i64_to_string reused buffer";
    expect(eq(s1.len, int64_t{1}));
    expect(eq(s2.len, int64_t{1}));
    expect(s1.ptr[0] == '0');
    expect(s2.ptr[0] == '1');
  };

  // After two successive conversions, the first result must still
  // read as its original value.  The old thread-local-buffer design
  // would have silently overwritten s1 with s2's contents.
  "i64_to_string prior result survives later call"_test = [] {
    auto s1 = __dao_conv_i64_to_string(42);
    auto s2 = __dao_conv_i64_to_string(1337);
    (void)s2;
    expect(eq(s1.len, int64_t{2}));
    expect(s1.ptr[0] == '4' && s1.ptr[1] == '2');
  };

  // All the sibling hooks follow the same convention.
  "i32_to_string returns distinct buffers"_test = [] {
    auto s1 = __dao_conv_i32_to_string(10);
    auto s2 = __dao_conv_i32_to_string(20);
    expect(s1.ptr != s2.ptr);
    expect(s1.ptr[0] == '1' && s1.ptr[1] == '0');
    expect(s2.ptr[0] == '2' && s2.ptr[1] == '0');
  };

  "u64_to_string returns distinct buffers"_test = [] {
    auto s1 = __dao_conv_u64_to_string(UINT64_C(7));
    auto s2 = __dao_conv_u64_to_string(UINT64_C(8));
    expect(s1.ptr != s2.ptr);
    expect(s1.ptr[0] == '7');
    expect(s2.ptr[0] == '8');
  };

  "f64_to_string returns distinct buffers"_test = [] {
    auto s1 = __dao_conv_f64_to_string(1.5);
    auto s2 = __dao_conv_f64_to_string(2.5);
    expect(s1.ptr != s2.ptr);
  };

  // bool_to_string returns a fresh buffer owned by the current domain,
  // like every other conversion.  Verify the two distinct strings.
  "bool_to_string returns fresh buffers"_test = [] {
    auto st = __dao_conv_bool_to_string(true);
    auto sf = __dao_conv_bool_to_string(false);
    expect(eq(st.len, int64_t{4}));
    expect(eq(sf.len, int64_t{5}));
    expect(std::memcmp(st.ptr, "true", 4) == 0);
    expect(std::memcmp(sf.ptr, "false", 5) == 0);
  };
};

// NOLINTEND(readability-magic-numbers)

auto main() -> int {} // NOLINT(readability-named-parameter)
