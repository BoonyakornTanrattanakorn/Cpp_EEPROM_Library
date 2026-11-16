#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "eeprom/crc32.hpp"

using eeprom::soft_crc32;

TEST(Crc32, StandardCheckValue) {
    const char* s = "123456789";
    EXPECT_EQ(soft_crc32(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

TEST(Crc32, KnownVectors) {
    EXPECT_EQ(soft_crc32(nullptr, 0), 0u);
    const std::vector<uint8_t> zeros(32, 0);
    EXPECT_EQ(soft_crc32(zeros.data(), zeros.size()), 0x190A55ADu);
    const char* fox = "The quick brown fox jumps over the lazy dog";
    EXPECT_EQ(soft_crc32(reinterpret_cast<const uint8_t*>(fox), std::strlen(fox)), 0x414FA339u);
}

TEST(Crc32, DetectsEverySingleBitFlip) {
    std::vector<uint8_t> data(64);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 37u + 5u);
    const uint32_t good = soft_crc32(data.data(), data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        for (unsigned b = 0; b < 8; ++b) {
            data[i] = static_cast<uint8_t>(data[i] ^ (1u << b));
            EXPECT_NE(soft_crc32(data.data(), data.size()), good) << "byte " << i << " bit " << b;
            data[i] = static_cast<uint8_t>(data[i] ^ (1u << b));
        }
    }
}
