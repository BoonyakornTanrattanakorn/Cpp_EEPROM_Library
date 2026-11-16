// Tests of the test support itself: the injection has to be trustworthy for the power-cut tests to mean anything.

#include <gtest/gtest.h>

#include "fake_device.hpp"

using eeprom::test::FakeDevice;

TEST(FakeDevice, BlankIsFFAndRoundTrips) {
    FakeDevice d(256);
    uint8_t b[4] = {};
    ASSERT_TRUE(d.read(10, b, 4));
    for (uint8_t x : b) EXPECT_EQ(x, 0xFF);
    const uint8_t w[4] = {1, 2, 3, 4};
    ASSERT_TRUE(d.write(10, w, 4));
    ASSERT_TRUE(d.read(10, b, 4));
    EXPECT_EQ(b[2], 3);
}

TEST(FakeDevice, RejectsOutOfRange) {
    FakeDevice d(64);
    uint8_t b[8] = {};
    EXPECT_FALSE(d.read(60, b, 8));
    EXPECT_FALSE(d.write(60, b, 8));
    EXPECT_TRUE(d.write(56, b, 8));
}

TEST(FakeDevice, PowerCutStoresExactlyNBytesThenFails) {
    for (size_t n = 0; n <= 8; ++n) {
        FakeDevice d(64);
        const uint8_t w[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        d.cut_after(n);
        const bool ok = d.write(0, w, 8);
        EXPECT_EQ(ok, n >= 8) << n;
        for (size_t i = 0; i < 8; ++i) EXPECT_EQ(d.memory()[i], i < n ? w[i] : 0xFF) << n << "/" << i;
        if (n < 8) {
            EXPECT_FALSE(d.powered());
            uint8_t r;
            EXPECT_FALSE(d.read(0, &r, 1)) << "device is off until restore_power()";
            EXPECT_FALSE(d.write(0, w, 1));
        }
        d.restore_power();
        uint8_t r = 0;
        EXPECT_TRUE(d.read(0, &r, 1));
        EXPECT_EQ(r, n > 0 ? 1 : 0xFF);
    }
}

TEST(FakeDevice, CutBudgetSpansWrites) {
    FakeDevice d(64);
    const uint8_t w[4] = {9, 9, 9, 9};
    d.cut_after(6);
    EXPECT_TRUE(d.write(0, w, 4));
    EXPECT_FALSE(d.write(8, w, 4)); // 2 of 4 bytes land
    EXPECT_EQ(d.memory()[8], 9);
    EXPECT_EQ(d.memory()[9], 9);
    EXPECT_EQ(d.memory()[10], 0xFF);
}

TEST(FakeDevice, BitFlipInMemoryAndOnWrite) {
    FakeDevice d(64);
    d.flip_bit(5, 3);
    EXPECT_EQ(d.memory()[5], 0xF7);

    const uint8_t w[4] = {0, 0, 0, 0};
    d.flip_on_write(2, 0x80);
    ASSERT_TRUE(d.write(0, w, 4));
    EXPECT_EQ(d.memory()[1], 0);
    EXPECT_EQ(d.memory()[2], 0x80);
    EXPECT_EQ(d.memory()[3], 0);
    ASSERT_TRUE(d.write(0, w, 4)); // one-shot
    EXPECT_EQ(d.memory()[2], 0);
}

TEST(FakeDevice, CountsPageWriteCycles) {
    FakeDevice d(256, 64);
    uint8_t w[100] = {};
    ASSERT_TRUE(d.write(60, w, 10)); // pages 0 and 1
    ASSERT_TRUE(d.write(0, w, 64));  // page 0
    EXPECT_EQ(d.page_writes(0), 2u);
    EXPECT_EQ(d.page_writes(1), 1u);
    EXPECT_EQ(d.page_writes(2), 0u);
    EXPECT_EQ(d.bytes_written(), 74u);
    EXPECT_EQ(d.write_calls(), 2u);
    d.reset_stats();
    EXPECT_EQ(d.page_writes(0), 0u);
}
