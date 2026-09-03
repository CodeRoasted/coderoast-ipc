//
// Single gtest entry point for the consumer unit-test executable — every
// other TU in tests/unit/ holds only TESTs (link GTest::gtest, not gtest_main).

#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
