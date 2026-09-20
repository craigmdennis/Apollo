/**
 * @file tests/unit/test_mic_store.cpp
 * @brief Test src/mic_store.*.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <src/mic_store.h>

namespace {
  class MicStore: public testing::Test {
  protected:
    void SetUp() override {
      file = std::filesystem::temp_directory_path() / ("mic_state_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".json");
      std::filesystem::remove(file);
    }

    void TearDown() override {
      std::filesystem::remove(file);
    }

    std::string file_text() const {
      std::ifstream in {file};
      std::stringstream text;
      text << in.rdbuf();
      return text.str();
    }

    std::filesystem::path file;
  };
}  // namespace

TEST_F(MicStore, MissingFileLoadsAsEmpty) {
  mic::store_t store {file};
  EXPECT_TRUE(store.load());
  EXPECT_TRUE(store.devices().empty());
}

TEST_F(MicStore, RoundTripsDevicesAndPreviousDefault) {
  mic::store_t store {file};
  store.add("Living room iPhone", "uuid-1", "token-one");
  store.previous_default_capture = "{device-id}";
  ASSERT_TRUE(store.save());

  mic::store_t reloaded {file};
  ASSERT_TRUE(reloaded.load());
  ASSERT_EQ(reloaded.devices().size(), 1u);
  EXPECT_EQ(reloaded.devices()[0].name, "Living room iPhone");
  EXPECT_EQ(reloaded.devices()[0].uuid, "uuid-1");
  EXPECT_EQ(reloaded.previous_default_capture, "{device-id}");
}

TEST_F(MicStore, AuthorizesOnlyTheMatchingToken) {
  mic::store_t store {file};
  store.add("MacBook Pro", "uuid-2", "token-two");
  auto device = store.authorize("token-two");
  ASSERT_TRUE(device.has_value());
  EXPECT_EQ(device->uuid, "uuid-2");
  EXPECT_FALSE(store.authorize("token-three").has_value());
  EXPECT_FALSE(store.authorize("").has_value());
}

TEST_F(MicStore, NeverWritesThePlainToken) {
  mic::store_t store {file};
  store.add("MacBook Pro", "uuid-2", "plain-secret-token");
  ASSERT_TRUE(store.save());
  EXPECT_EQ(file_text().find("plain-secret-token"), std::string::npos);
  EXPECT_NE(file_text().find(mic::store_t::hash_token("plain-secret-token")), std::string::npos);
}

TEST_F(MicStore, RemovesByUuid) {
  mic::store_t store {file};
  store.add("A", "uuid-a", "token-a");
  store.add("B", "uuid-b", "token-b");
  EXPECT_TRUE(store.remove("uuid-a"));
  EXPECT_FALSE(store.remove("uuid-a"));
  ASSERT_EQ(store.devices().size(), 1u);
  EXPECT_EQ(store.devices()[0].uuid, "uuid-b");
  EXPECT_FALSE(store.authorize("token-a").has_value());
}

TEST_F(MicStore, CorruptFileReturnsFalseAndStaysEmpty) {
  {
    std::ofstream out {file};
    out << "{ not json";
  }
  mic::store_t store {file};
  EXPECT_FALSE(store.load());
  EXPECT_TRUE(store.devices().empty());
}
