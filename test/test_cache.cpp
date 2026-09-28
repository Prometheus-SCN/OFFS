//
// Created by victor on 9/28/26.
//
// Tests for the pure helpers behind `offs cache size/move`. No daemon is
// spawned: everything here exercises cli_util.c's exported cache helpers
// against real temp files and directories.

#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>  /* usleep */
#endif

extern "C" {
#include "cli_util.h"
#include "Util/file_copy.h"
#include "Util/mkdir_p.h"
#include "Util/rm_rf.h"
}

/* MSVC CRT has no setenv; test_cli.cpp defines the same shims. */
#ifdef _WIN32
static inline void offs_setenv(const char* name, const char* value) {
  _putenv_s(name, value);
}
static inline void offs_unsetenv(const char* name) {
  _putenv_s(name, "");
}
#define setenv(name, value, overwrite) offs_setenv(name, value)
#define unsetenv(name) offs_unsetenv(name)
#endif

/* Unique per call: two tests sharing a tag never touch the same directory.
   The suffix also dodges the Windows asynchronous-delete race, where a
   just-rm_rf'd name is still pending-delete when mkdir_p runs. */
static int g_dir_seq = 0;

static std::string temp_dir(const char* tag) {
#ifdef _WIN32
  char base[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, base);
  EXPECT_GT(n, 0u);
  std::string dir = std::string(base) + "offs-cache-test-" + tag + "-" +
                    std::to_string(++g_dir_seq);
#else
  std::string dir = std::string("/tmp/offs-cache-test-") + tag + "-" +
                    std::to_string(++g_dir_seq);
#endif
  /* Retry briefly: on Windows the previous deletion may still be in flight. */
  for (int attempt = 0; attempt < 20; attempt++) {
    rm_rf(dir.c_str());
    if (mkdir_p((char*)dir.c_str()) == 0 && dir_is_empty(dir.c_str()) == 1) {
      return dir;
    }
#ifdef _WIN32
    Sleep(50);
#else
    usleep(50000);
#endif
  }
  EXPECT_TRUE(false) << "could not create temp dir " << dir;
  return dir;
}

static bool write_file(const std::string& path, const char* content) {
  FILE* file = fopen(path.c_str(), "wb");
  if (file == NULL) return false;
  fputs(content, file);
  fclose(file);
  return true;
}

static bool read_file(const std::string& path, std::string* out) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == NULL) return false;
  char buffer[1024];
  size_t n = fread(buffer, 1, sizeof(buffer), file);
  fclose(file);
  out->assign(buffer, n);
  return true;
}

static bool file_exists(const std::string& path) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == NULL) return false;
  fclose(file);
  return true;
}

static void make_dir(const std::string& path) {
  ASSERT_EQ(mkdir_p((char*)path.c_str()), 0);
}

/* --- cli_cache_paths_conflict -------------------------------------------- */

TEST(CachePathsConflictTest, IdenticalPaths) {
  EXPECT_EQ(cli_cache_paths_conflict("C:\\offs", "C:\\offs"), 1);
}

TEST(CachePathsConflictTest, SameAfterNormalization) {
  EXPECT_EQ(cli_cache_paths_conflict("C:\\offs\\", "C:\\offs"), 1);
  EXPECT_EQ(cli_cache_paths_conflict("C:\\offs\\\\sub", "C:\\offs\\sub"), 1);
  EXPECT_EQ(cli_cache_paths_conflict("C:/offs/sub", "C:\\offs\\sub"), 1);
}

TEST(CachePathsConflictTest, CaseInsensitiveOnWindows) {
#ifdef _WIN32
  EXPECT_EQ(cli_cache_paths_conflict("C:\\OFFS", "c:\\offs"), 1);
#else
  EXPECT_EQ(cli_cache_paths_conflict("/offs", "/OFFS"), 0);
#endif
}

TEST(CachePathsConflictTest, NestedBothWays) {
  EXPECT_EQ(cli_cache_paths_conflict("C:\\offs", "C:\\offs\\blocks"), 2);
  EXPECT_EQ(cli_cache_paths_conflict("C:\\offs\\blocks", "C:\\offs"), 2);
}

TEST(CachePathsConflictTest, SiblingIsDisjoint) {
  EXPECT_EQ(cli_cache_paths_conflict("C:\\offs", "C:\\offs2"), 0);
}

TEST(CachePathsConflictTest, PosixStyle) {
#ifndef _WIN32
  EXPECT_EQ(cli_cache_paths_conflict("/offs", "/offs/blocks"), 2);
  EXPECT_EQ(cli_cache_paths_conflict("/offs", "/offs-blocks"), 0);
#endif
}

/* --- cli_cache_read_config_file_dirs -------------------------------------- */

TEST(CacheConfigFileDirsTest, ReadsBothDirs) {
  std::string dir = temp_dir("cfg");
  std::string path = dir + "/offs.json";
  ASSERT_TRUE(write_file(path,
    "{\"daemon\":{\"config-dir\":\"C:\\\\cfgdir\"},\"cache\":{\"dir\":\"C:\\\\cachedir\"}}"));
  char cfg[256], cache[256];
  ASSERT_EQ(cli_cache_read_config_file_dirs(path.c_str(), cfg, sizeof(cfg),
                                            cache, sizeof(cache)), 0);
  EXPECT_STREQ(cfg, "C:\\cfgdir");
  EXPECT_STREQ(cache, "C:\\cachedir");
}

TEST(CacheConfigFileDirsTest, AbsentKeysLeaveOutputsEmpty) {
  std::string dir = temp_dir("cfg2");
  std::string path = dir + "/offs.json";
  ASSERT_TRUE(write_file(path, "{}"));
  char cfg[256], cache[256];
  ASSERT_EQ(cli_cache_read_config_file_dirs(path.c_str(), cfg, sizeof(cfg),
                                            cache, sizeof(cache)), 0);
  EXPECT_STREQ(cfg, "");
  EXPECT_STREQ(cache, "");
}

TEST(CacheConfigFileDirsTest, MissingFileFails) {
  char cfg[256], cache[256];
  EXPECT_EQ(cli_cache_read_config_file_dirs("Z:\\no\\such\\offs.json",
                                            cfg, sizeof(cfg), cache, sizeof(cache)), -1);
}

TEST(CacheConfigFileDirsTest, BadJsonFails) {
  std::string dir = temp_dir("cfg3");
  std::string path = dir + "/offs.json";
  ASSERT_TRUE(write_file(path, "not json"));
  char cfg[256], cache[256];
  EXPECT_EQ(cli_cache_read_config_file_dirs(path.c_str(), cfg, sizeof(cfg),
                                            cache, sizeof(cache)), -1);
}

/* --- cli_cache_move_tree --------------------------------------------------- */

TEST(CacheMoveTreeTest, HappyPathPreservesContentsAndEmptiesSource) {
  std::string base = temp_dir("mv");
  std::string src = base + "/src";
  std::string dst = base + "/dst";
  make_dir(src + "/sub");
  ASSERT_TRUE(write_file(src + "/a.txt", "alpha"));
  ASSERT_TRUE(write_file(src + "/sub/b.txt", "bravo"));

  char failed[256] = {0};
  ASSERT_EQ(cli_cache_move_tree(src.c_str(), dst.c_str(), FILE_VERIFY_SHA256,
                                failed, sizeof(failed)), 0);
  EXPECT_STREQ(failed, "");

  std::string content;
  ASSERT_TRUE(read_file(dst + "/a.txt", &content));
  EXPECT_EQ(content, "alpha");
  ASSERT_TRUE(read_file(dst + "/sub/b.txt", &content));
  EXPECT_EQ(content, "bravo");

  /* Everything moved out; only the source shell remains. */
  EXPECT_EQ(dir_is_empty(src.c_str()), 1);
}

TEST(CacheMoveTreeTest, UnsafeModeMovesWithoutVerify) {
  std::string base = temp_dir("mvu");
  std::string src = base + "/src";
  std::string dst = base + "/dst";
  make_dir(src);
  ASSERT_TRUE(write_file(src + "/a.txt", "alpha"));

  char failed[256] = {0};
  ASSERT_EQ(cli_cache_move_tree(src.c_str(), dst.c_str(), FILE_VERIFY_NONE,
                                failed, sizeof(failed)), 0);
  std::string content;
  ASSERT_TRUE(read_file(dst + "/a.txt", &content));
  EXPECT_EQ(content, "alpha");
  EXPECT_EQ(dir_is_empty(src.c_str()), 1);
}

TEST(CacheMoveTreeTest, FailureReportsPathAndKeepsSourceFile) {
  std::string base = temp_dir("mvf");
  std::string src = base + "/src";
  std::string dst = base + "/dst";
  make_dir(src + "/d");
  ASSERT_TRUE(write_file(src + "/b.txt", "blocked"));
  ASSERT_TRUE(write_file(src + "/d/c.txt", "charlie"));

  /* A directory planted at the destination file path makes the per-file
     move of b.txt fail (the destination cannot be opened for writing). */
  make_dir(dst + "/b.txt");

  char failed[256] = {0};
  ASSERT_EQ(cli_cache_move_tree(src.c_str(), dst.c_str(), FILE_VERIFY_SHA256,
                                failed, sizeof(failed)), -1);
  EXPECT_TRUE(strstr(failed, "b.txt") != NULL) << "failed_path: " << failed;

  /* The blocked file stayed in the source. */
  EXPECT_TRUE(file_exists(src + "/b.txt"));
  /* The planted directory is untouched (not overwritten by a file). */
  EXPECT_EQ(dir_is_empty((dst + "/b.txt").c_str()), 1);
}

TEST(CacheMoveTreeTest, NestedDirectoryFailureAlsoStops) {
  std::string base = temp_dir("mvn");
  std::string src = base + "/src";
  std::string dst = base + "/dst";
  make_dir(src + "/sub");
  ASSERT_TRUE(write_file(src + "/sub/c.txt", "charlie"));
  /* Block the nested directory's destination. */
  make_dir(dst + "/sub/c.txt");

  char failed[256] = {0};
  ASSERT_EQ(cli_cache_move_tree(src.c_str(), dst.c_str(), FILE_VERIFY_SHA256,
                                failed, sizeof(failed)), -1);
  EXPECT_TRUE(strstr(failed, "c.txt") != NULL) << "failed_path: " << failed;
  EXPECT_TRUE(file_exists(src + "/sub/c.txt"));
}

/* --- cli_cache_resolve_current --------------------------------------------- */

TEST(CacheResolveCurrentTest, FromFlagWins) {
  char out[256];
  ASSERT_EQ(cli_cache_resolve_current("C:\\fromdir", NULL, out, sizeof(out)), 0);
  EXPECT_STREQ(out, "C:\\fromdir");
}

TEST(CacheResolveCurrentTest, ConfigFileCacheDirWins) {
  std::string dir = temp_dir("res");
  std::string path = dir + "/offs.json";
  ASSERT_TRUE(write_file(path, "{\"cache\":{\"dir\":\"D:\\\\filecache\"}}"));
  setenv("OFFS_CONFIG", path.c_str(), 1);

  char out[256];
  ASSERT_EQ(cli_cache_resolve_current(NULL, NULL, out, sizeof(out)), 0);
  EXPECT_STREQ(out, "D:\\filecache");
  unsetenv("OFFS_CONFIG");
}

TEST(CacheResolveCurrentTest, PendingWinsWhenConfigFileHasNoCacheDir) {
  std::string dir = temp_dir("resp");
  /* $OFFS_CONFIG points at a file that does not exist, so the config-file
     branch falls through to the pending config in config_dir. */
  setenv("OFFS_CONFIG", (dir + "/missing.json").c_str(), 1);
  ASSERT_TRUE(write_file(dir + "/pending_config.json",
                         "{\"cache_dir\":\"D:\\\\pending\"}"));

  char out[256];
  ASSERT_EQ(cli_cache_resolve_current(NULL, dir.c_str(), out, sizeof(out)), 0);
  EXPECT_STREQ(out, "D:\\pending");
  unsetenv("OFFS_CONFIG");
}

TEST(CacheResolveCurrentTest, FallsBackToPlatformDefault) {
  unsetenv("OFFS_CONFIG");
  char out[1024];
  ASSERT_EQ(cli_cache_resolve_current(NULL, NULL, out, sizeof(out)), 0);
  EXPECT_STRNE(out, "");
}