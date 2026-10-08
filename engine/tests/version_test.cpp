#include "av/version.h"

#include <gtest/gtest.h>

#include <string>

TEST(Version, IsNonEmpty) { EXPECT_FALSE(std::string(av::engineVersion()).empty()); }
