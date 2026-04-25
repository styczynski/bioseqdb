#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <string_view>
#include <sqlgen/postgres.hpp>
#include "test_helpers.hpp"

namespace test_listen_notify {

using namespace sqlgen;
using namespace sqlgen::postgres;

std::list<Notification> wait_for_notifications(auto& conn,
  std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::list<Notification> all;

  while (std::chrono::steady_clock::now() < deadline) {
    auto batch = conn->get_notifications();
    if (!batch.empty()) {
      all.splice(all.end(), batch);  // efficient for list
      // Continue looping briefly in case more arrived
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  return all;
}

TEST(postgres, basic_listen_notify) {
  using namespace sqlgen;
  using namespace sqlgen::postgres;

  const auto credentials = sqlgen::postgres::test::make_credentials();

  auto listener_result = sqlgen::postgres::connect(credentials);
  auto sender_result = sqlgen::postgres::connect(credentials);

  ASSERT_TRUE(listener_result);
  ASSERT_TRUE(sender_result);

  auto listener = listener_result.value();
  auto sender = sender_result.value();

  // Listener subscribes to channel
  auto listen_res = listener->listen("test_channel");
  ASSERT_TRUE(listen_res) << listen_res.error().what();

  // Sender sends a notification
  auto notify_res = sender->notify("test_channel", "hello world");
  ASSERT_TRUE(notify_res) << notify_res.error().what();

  // Listener waits and receives
  auto notifications = wait_for_notifications(listener);
  ASSERT_EQ(notifications.size(), 1);
  EXPECT_EQ(notifications.front().channel, "test_channel");
  EXPECT_EQ(notifications.front().payload, "hello world");
  EXPECT_GT(notifications.front().backend_pid, 0);
}

TEST(postgres, notify_without_listener_is_silent) {
  using namespace sqlgen;
  using namespace sqlgen::postgres;

  const auto credentials = sqlgen::postgres::test::make_credentials();

  auto sender_result = sqlgen::postgres::connect(credentials);

  ASSERT_TRUE(sender_result);

  auto sender = sender_result.value();

  // Notify on a channel with no listener → should not error
  auto res = sender->notify("unused_channel", "payload");
  ASSERT_TRUE(res) << res.error().what();
}

TEST(postgres, InvalidChannelNameRejected) {
  using namespace sqlgen;
  using namespace sqlgen::postgres;

  const auto credentials = sqlgen::postgres::test::make_credentials();

  auto listener_result = sqlgen::postgres::connect(credentials);
  auto sender_result = sqlgen::postgres::connect(credentials);

  ASSERT_TRUE(listener_result);
  ASSERT_TRUE(sender_result);

  auto listener = listener_result.value();
  auto sender = sender_result.value();

  auto conn = postgres::connect(credentials);
  ASSERT_TRUE(conn);

  // Invalid: starts with digit
  EXPECT_FALSE(listener->listen("123chan"));
  EXPECT_FALSE(sender->notify("123chan"));

  // Invalid: contains hyphen
  EXPECT_FALSE(listener->listen("my-chan"));
  EXPECT_FALSE(sender->notify("my-chan"));

  // Valid: underscore + alphanumeric
  EXPECT_TRUE(listener->listen("_chan1"));
  EXPECT_TRUE(sender->unlisten("_chan1"));
}

TEST(postgres, unlisten_star) {
  using namespace sqlgen;
  using namespace sqlgen::postgres;

  const auto credentials = sqlgen::postgres::test::make_credentials();

  auto listener_result = sqlgen::postgres::connect(credentials);

  ASSERT_TRUE(listener_result);

  auto listener = listener_result.value();

  ASSERT_TRUE(listener->listen("chan_a"));
  ASSERT_TRUE(listener->listen("chan_b"));

  // Unlisten all
  ASSERT_TRUE(listener->unlisten("*"));

  // Notify won't be received, but we just verify no error
  auto sender = postgres::connect(credentials);
  ASSERT_TRUE(listener->notify("chan_a", "test"));
}

TEST(postgres, multiple_notifications_in_burst) {
  using namespace sqlgen;
  using namespace sqlgen::postgres;

  const auto credentials = sqlgen::postgres::test::make_credentials();

  auto listener_result = sqlgen::postgres::connect(credentials);
  auto sender_result = sqlgen::postgres::connect(credentials);

  ASSERT_TRUE(listener_result);
  ASSERT_TRUE(sender_result);

  auto listener = listener_result.value();
  auto sender = sender_result.value();

  const std::string channel = "burst_channel";
  ASSERT_TRUE(listener->listen(channel));

  const int num_notifications = 5;
  std::list<std::string> expected_payloads;
  for (int i = 0; i < num_notifications; ++i) {
    const std::string payload = "msg_" + std::to_string(i);
    expected_payloads.push_back(payload);
    ASSERT_TRUE(sender->notify(channel, payload));
    // Small delay to improve reliability on slow CI (optional but safe)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Drain all notifications with retry
  std::list<sqlgen::postgres::Notification> notifications;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (notifications.size() < num_notifications && std::chrono::steady_clock::now() < deadline) {
    auto batch = wait_for_notifications(listener, std::chrono::milliseconds{100});
    std::move(batch.begin(), batch.end(), std::back_inserter(notifications));
  }

  ASSERT_EQ(notifications.size(), expected_payloads.size());

  auto expected_it = expected_payloads.begin();
  auto notify_it = notifications.begin();
  int i = 0;

  for (; expected_it != expected_payloads.end(); ++expected_it, ++notify_it, ++i) {
    EXPECT_EQ(notify_it->channel, channel) << "Notification #" << i;
    EXPECT_EQ(notify_it->payload, *expected_it) << "Notification #" << i;
    EXPECT_GT(notify_it->backend_pid, 0) << "Notification #" << i;
  }
}

}

#endif
