#include <gtest/gtest.h>

#include <string>

#include "av/version.h"

TEST(Version, IsNonEmpty) { EXPECT_FALSE(std::string(av::engineVersion()).empty()); }
