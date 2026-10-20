#include <gtest/gtest.h>

#include "me/me.hpp"

TEST(Skeleton, BuildsWithCpp20) { EXPECT_GE(__cplusplus, 202002L); }
