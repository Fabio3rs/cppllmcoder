#include <gtest/gtest.h>

#include "utils/utf8_text.hpp"

#include <string>

TEST(Utf8Text, PrefixByBytesKeepsAsciiWithinBudget) {
    const std::string text = "hello";
    EXPECT_EQ(utf8::prefix_by_bytes(text, 3), "hel");
}

TEST(Utf8Text, PrefixByBytesPreservesCombiningCharacterCluster) {
    const std::string text = "e"
                             "\xCC\x81"
                             "x";
    EXPECT_TRUE(utf8::prefix_by_bytes(text, 2).empty());
    EXPECT_EQ(utf8::prefix_by_bytes(text, 3), "e"
                                              "\xCC\x81");
}

TEST(Utf8Text, PrefixByBytesPreservesEmojiModifierCluster) {
    const std::string text = "\xF0\x9F\x91\x8D"
                             "\xF0\x9F\x8F\xBD"
                             "!";
    EXPECT_TRUE(utf8::prefix_by_bytes(text, 4).empty());
    EXPECT_EQ(utf8::prefix_by_bytes(text, 8), "\xF0\x9F\x91\x8D"
                                              "\xF0\x9F\x8F\xBD");
}

TEST(Utf8Text, SliceByBytesAlignsUnsafeOffsetsToNextBoundary) {
    const std::string text = "a"
                             "\xC3\xA9"
                             "\xF0\x9F\x98\x80"
                             "b";
    EXPECT_EQ(utf8::slice_by_bytes(text, 2, 4), "\xF0\x9F\x98\x80");
}

TEST(Utf8Text, TruncateForDisplayAppendsSuffixAfterSafeBoundary) {
    const std::string text = "a"
                             "\xC3\xA9"
                             "\xF0\x9F\x98\x80"
                             "z";
    EXPECT_EQ(utf8::truncate_for_display(text, 5), "a"
                                                   "\xC3\xA9"
                                                   "...");
}
