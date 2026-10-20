#include "me/me.hpp"

#include <gtest/gtest.h>

TEST(Skeleton, BuildsWithCpp20) { EXPECT_GE(__cplusplus, 202002L); }
