//
// Created by victor on 5/28/26.
//

#include <gtest/gtest.h>
#include <cstdio>
#include <string>

extern "C" {
#include "cli_util.h"
#include "l10n/en.h"
#include "client.h"
#include "Util/allocator.h"
#include <stdlib.h>
}

TEST(L10NTest, AllStringsNonEmpty) {
  EXPECT_STRNE(L10N_CLI_DESCRIPTION, "");
  EXPECT_STRNE(L10N_COMMANDS, "");
  EXPECT_STRNE(L10N_USAGE, "");
  EXPECT_STRNE(L10N_UNKNOWN_COMMAND, "");
  EXPECT_STRNE(L10N_DAEMON_UNREACHABLE, "");
  EXPECT_STRNE(L10N_ERROR, "");
  EXPECT_STRNE(L10N_OK, "");
}

TEST(L10NTest, CommandDescriptionsNonEmpty) {
  EXPECT_STRNE(L10N_START_DESC, "");
  EXPECT_STRNE(L10N_STOP_DESC, "");
  EXPECT_STRNE(L10N_RESTART_DESC, "");
  EXPECT_STRNE(L10N_PUT_DESC, "");
  EXPECT_STRNE(L10N_GET_DESC, "");
  EXPECT_STRNE(L10N_BLOCK_DESC, "");
  EXPECT_STRNE(L10N_PEER_DESC, "");
  EXPECT_STRNE(L10N_CONFIG_DESC, "");
  EXPECT_STRNE(L10N_FRIEND_DESC, "");
  EXPECT_STRNE(L10N_HEALTH_DESC, "");
  EXPECT_STRNE(L10N_STATUS_DESC, "");
  EXPECT_STRNE(L10N_VERSION_DESC, "");
  EXPECT_STRNE(L10N_HELP_DESC, "");
}

TEST(L10NTest, HealthLabelsNonEmpty) {
  EXPECT_STRNE(L10N_HEALTH_STATUS, "");
  EXPECT_STRNE(L10N_HEALTH_UPTIME, "");
  EXPECT_STRNE(L10N_HEALTH_BLOCKS, "");
  EXPECT_STRNE(L10N_HEALTH_CACHE_SIZE, "");
  EXPECT_STRNE(L10N_HEALTH_PEERS, "");
}

TEST(L10NTest, UsageStringsNonEmpty) {
  EXPECT_STRNE(L10N_PUT_USAGE, "");
  EXPECT_STRNE(L10N_GET_USAGE, "");
  EXPECT_STRNE(L10N_BLOCK_PUT_USAGE, "");
  EXPECT_STRNE(L10N_BLOCK_GET_USAGE, "");
  EXPECT_STRNE(L10N_BLOCK_DELETE_USAGE, "");
  EXPECT_STRNE(L10N_PEER_CONNECT_USAGE, "");
  EXPECT_STRNE(L10N_FRIEND_ADD_USAGE, "");
  EXPECT_STRNE(L10N_FRIEND_REMOVE_USAGE, "");
}

TEST(L10NTest, PromptStringsNonEmpty) {
  EXPECT_STRNE(L10N_PEER_INFO_PROMPT, "");
  EXPECT_STRNE(L10N_PEER_LIST_PROMPT, "");
  EXPECT_STRNE(L10N_CONFIG_SHOW_PROMPT, "");
  EXPECT_STRNE(L10N_FRIEND_LIST_PROMPT, "");
}

TEST(L10NTest, DaemonMessagesNonEmpty) {
  EXPECT_STRNE(L10N_DAEMON_STARTED, "");
  EXPECT_STRNE(L10N_DAEMON_STOPPED, "");
  EXPECT_STRNE(L10N_DAEMON_ALREADY_RUNNING, "");
}

TEST(CommandTableTest, AllEntriesHaveName) {
  const cli_command_t* commands = cli_command_table();
  ASSERT_NE(commands, nullptr);
  for (int i = 0; commands[i].name != NULL; i++) {
    EXPECT_STRNE(commands[i].name, "");
  }
}

TEST(CommandTableTest, AllEntriesHaveDescription) {
  const cli_command_t* commands = cli_command_table();
  ASSERT_NE(commands, nullptr);
  for (int i = 0; commands[i].name != NULL; i++) {
    EXPECT_STRNE(commands[i].description, "");
  }
}

TEST(CommandTableTest, AllEntriesHaveHandlerExceptHelp) {
  const cli_command_t* commands = cli_command_table();
  ASSERT_NE(commands, nullptr);
  for (int i = 0; commands[i].name != NULL; i++) {
    if (strcmp(commands[i].name, "help") == 0) {
      EXPECT_EQ(commands[i].handler, nullptr);
    } else {
      EXPECT_NE(commands[i].handler, nullptr);
    }
  }
}

TEST(CommandTableTest, ExpectedCommandsExist) {
  const cli_command_t* commands = cli_command_table();
  ASSERT_NE(commands, nullptr);

  const char* expected[] = {
    "start", "stop", "restart", "put", "get", "block", "peer",
    "config", "friend", "health", "status", "version", "help", NULL
  };

  for (int exp = 0; expected[exp] != NULL; exp++) {
    bool found = false;
    for (int i = 0; commands[i].name != NULL; i++) {
      if (strcmp(commands[i].name, expected[exp]) == 0) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << "Missing command: " << expected[exp];
  }
}

TEST(CommandTableTest, TableCount) {
  const cli_command_t* commands = cli_command_table();
  ASSERT_NE(commands, nullptr);
  int count = 0;
  while (commands[count].name != NULL) count++;
  EXPECT_EQ(count, 13);
}

TEST(LangDetectionTest, ReturnsEnWhenNoEnvVars) {
  unsetenv("OFFS_LANG");
  unsetenv("LANG");
  const char* lang = cli_detect_lang();
  EXPECT_STREQ(lang, "en");
}

TEST(LangDetectionTest, RespectsOffsLangEnv) {
  setenv("OFFS_LANG", "fr", 1);
  const char* lang = cli_detect_lang();
  EXPECT_STREQ(lang, "fr");
  unsetenv("OFFS_LANG");
}

TEST(LangDetectionTest, FallsBackToLangEnv) {
  unsetenv("OFFS_LANG");
  setenv("LANG", "de_DE.UTF-8", 1);
  const char* lang = cli_detect_lang();
  EXPECT_STREQ(lang, "de_DE");
  unsetenv("LANG");
}

TEST(LangDetectionTest, StripsEncodingFromLang) {
  unsetenv("OFFS_LANG");
  setenv("LANG", "ja_JP.eucJP", 1);
  const char* lang = cli_detect_lang();
  EXPECT_STREQ(lang, "ja_JP");
  unsetenv("LANG");
}

TEST(LangDetectionTest, HandlesLangWithoutDot) {
  unsetenv("OFFS_LANG");
  setenv("LANG", "C", 1);
  const char* lang = cli_detect_lang();
  EXPECT_STREQ(lang, "C");
  unsetenv("LANG");
}

TEST(HelpOutputTest, PrintsAllCommands) {
  testing::internal::CaptureStdout();
  cli_print_help(NULL);
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_NE(output.find("OFFS"), std::string::npos);
  EXPECT_NE(output.find("start"), std::string::npos);
  EXPECT_NE(output.find("stop"), std::string::npos);
  EXPECT_NE(output.find("put"), std::string::npos);
  EXPECT_NE(output.find("get"), std::string::npos);
  EXPECT_NE(output.find("help"), std::string::npos);
}

TEST(HelpOutputTest, PrintsSpecificCommand) {
  testing::internal::CaptureStdout();
  cli_print_help("status");
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_NE(output.find("status"), std::string::npos);
  EXPECT_NE(output.find("Usage"), std::string::npos);
}

TEST(HelpOutputTest, PrintsErrorForUnknownCommand) {
  testing::internal::CaptureStdout();
  cli_print_help("nonexistent_cmd");
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_NE(output.find("Unknown"), std::string::npos);
  EXPECT_NE(output.find("nonexistent_cmd"), std::string::npos);
}
