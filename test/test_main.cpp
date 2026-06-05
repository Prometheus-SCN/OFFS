//
// Created by victor on 5/28/26.
//

#include <gtest/gtest.h>

extern "C" {
#include "Util/allocator.h"
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
