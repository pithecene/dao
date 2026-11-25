#include "support/sha256.h"

#include <boost/ut.hpp>

#include <string>

using namespace boost::ut;

// NOLINTBEGIN(readability-magic-numbers)

suite<"sha256"> sha256_suite = [] {
  "matches FIPS 180-4 standard test vectors and padding boundaries"_test = [] {
    expect(dao::sha256_hex("") ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect(dao::sha256_hex("abc") ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect(dao::sha256_hex(
               "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    expect(dao::sha256_hex(
               "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
               "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
           "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    const std::string exact_block(64, 'a');
    expect(dao::sha256_hex(exact_block) ==
           "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

    const std::string million_a(1000000, 'a');
    expect(dao::sha256_hex(million_a) ==
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  };
};

auto main(int argc, const char** argv) -> int {
  return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv}) ? 1 : 0;
}

// NOLINTEND(readability-magic-numbers)
