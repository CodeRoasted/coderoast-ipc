
// note: the one entry point of the consumer test binary; the other TUs hold only TESTs.
#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
