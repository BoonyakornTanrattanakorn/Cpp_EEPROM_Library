#include <gtest/gtest.h>

#include <vector>

#include "eeprom/writer.hpp"

using namespace eeprom;

TEST(Writer, EntryLayoutIsIdTypeLenValueLittleEndian) {
    uint8_t buf[64] = {};
    Writer w(buf, sizeof(buf));
    ASSERT_EQ(w.put_u32(0x0102, 0xAABBCCDDu), Status::Ok);
    ASSERT_EQ(w.put_bool(3, true), Status::Ok);
    ASSERT_EQ(w.put_i32(4, -2), Status::Ok);
    const uint8_t blob[3] = {7, 8, 9};
    ASSERT_EQ(w.put_blob(5, blob, 3), Status::Ok);
    const uint8_t expect[] = {
        0x02, 0x01, 0, 4, 0xDD, 0xCC, 0xBB, 0xAA, // u32
        0x03, 0x00, 3, 1, 1,                      // bool
        0x04, 0x00, 1, 4, 0xFE, 0xFF, 0xFF, 0xFF, // i32
        0x05, 0x00, 4, 3, 7,    8,    9,          // blob
    };
    ASSERT_EQ(w.size(), sizeof(expect));
    EXPECT_EQ(std::vector<uint8_t>(buf, buf + w.size()), std::vector<uint8_t>(expect, expect + sizeof(expect)));
}

TEST(Writer, FloatRoundTripsThroughDecoder) {
    uint8_t buf[16];
    Writer w(buf, sizeof(buf));
    ASSERT_EQ(w.put_f32(1, 12.5f), Status::Ok);
    EXPECT_EQ(buf[2], static_cast<uint8_t>(Type::F32));
    EXPECT_EQ(get_f32(buf + 4), 12.5f);
    EXPECT_EQ(get_u32(buf + 4), 0x41480000u);
}

TEST(Writer, DecodersHandleNegativeAndFullRange) {
    const uint8_t v[4] = {0xFE, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(get_i32(v), -2);
    EXPECT_EQ(get_u32(v), 0xFFFFFFFEu);
}

TEST(Writer, OverflowIsStickyAndLeavesBufferUsable) {
    uint8_t buf[10];
    Writer w(buf, sizeof(buf));
    ASSERT_EQ(w.put_u32(1, 1), Status::Ok); // 8 bytes
    EXPECT_EQ(w.put_u32(2, 2), Status::TooLarge);
    EXPECT_EQ(w.put_bool(3, true), Status::TooLarge) << "sticky even though it would not fit anyway";
    EXPECT_EQ(w.status(), Status::TooLarge);
    EXPECT_EQ(w.size(), 8u);
}

TEST(Writer, ExactFitIsAccepted) {
    uint8_t buf[8];
    Writer w(buf, sizeof(buf));
    EXPECT_EQ(w.put_u32(1, 1), Status::Ok);
    EXPECT_EQ(w.status(), Status::Ok);
}

TEST(Writer, BlobLengthLimit) {
    std::vector<uint8_t> buf(400), data(300, 1);
    Writer w(buf.data(), buf.size());
    EXPECT_EQ(w.put_blob(1, data.data(), 255), Status::Ok);
    Writer w2(buf.data(), buf.size());
    EXPECT_EQ(w2.put_blob(1, data.data(), 256), Status::BadArg);
    Writer w3(buf.data(), buf.size());
    EXPECT_EQ(w3.put_blob(1, nullptr, 0), Status::Ok) << "empty blob";
    EXPECT_EQ(w3.size(), 4u);
}
