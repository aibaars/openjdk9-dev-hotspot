/*
 * Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

/////////////// Unit tests ///////////////

#ifndef PRODUCT

#include "gc/shared/gcTraceTime.inline.hpp"
#include "logging/log.hpp"
#include "logging/logConfiguration.hpp"
#include "logging/logFileOutput.hpp"
#include "logging/logOutput.hpp"
#include "logging/logTagLevelExpression.hpp"
#include "logging/logTagSet.hpp"
#include "logging/logStream.inline.hpp"
#include "memory/resourceArea.hpp"

#define assert_str_eq(s1, s2) \
  assert(strcmp(s1, s2) == 0, "Expected '%s' to equal '%s'", s1, s2)

#define assert_char_in(c, s) \
  assert(strchr(s, c) != NULL, "Expected '%s' to contain character '%c'", s, c)

#define assert_char_not_in(c, s) \
  assert(strchr(s, c) == NULL, "Expected '%s' to *not* contain character '%c'", s, c)

void Test_log_tag_combinations_limit() {
  assert(LogTagLevelExpression::MaxCombinations > LogTagSet::ntagsets(),
      "Combination limit (" SIZE_FORMAT ") not sufficient "
      "for configuring all available tag sets (" SIZE_FORMAT ")",
      LogTagLevelExpression::MaxCombinations, LogTagSet::ntagsets());
}

class TestLogFile {
 private:
  char file_name[256];

  void set_name(const char* test_name) {
    const char* tmpdir = os::get_temp_directory();
    int pos = jio_snprintf(file_name, sizeof(file_name), "%s%svmtest.%s.%d.log", tmpdir, os::file_separator(), test_name, os::current_process_id());
    assert(pos > 0, "too small log file name buffer");
    assert((size_t)pos < sizeof(file_name), "too small log file name buffer");
  }

 public:
  TestLogFile(const char* test_name) {
    set_name(test_name);
    remove(name());
  }

  ~TestLogFile() {
    remove(name());
  }

  const char* name() {
    return file_name;
  }
};

class TestLogSavedConfig {
 private:
  char* _saved_config;
  char* _new_output;
  Log(logging) _log;
 public:
  TestLogSavedConfig(const char* apply_output = NULL, const char* apply_setting = NULL) : _new_output(0) {
    ResourceMark rm;
    _saved_config = os::strdup_check_oom(LogOutput::Stdout->config_string());
    bool success = LogConfiguration::parse_log_arguments("stdout", "all=off", NULL, NULL, _log.error_stream());
    assert(success, "test unable to turn all off");

    if (apply_output) {
      _new_output = os::strdup_check_oom(apply_output);
      bool success = LogConfiguration::parse_log_arguments(_new_output, apply_setting,  NULL, NULL, _log.error_stream());
      assert(success, "test unable to apply test log configuration");
    }
  }

  ~TestLogSavedConfig() {
    ResourceMark rm;
    if (_new_output) {
      bool success = LogConfiguration::parse_log_arguments(_new_output, "all=off", NULL, NULL, _log.error_stream());
      assert(success, "test unable to turn all off");
      os::free(_new_output);
    }

    bool success = LogConfiguration::parse_log_arguments("stdout", _saved_config, NULL, NULL, _log.error_stream());
    assert(success, "test unable to restore log configuration");
    os::free(_saved_config);
  }
};

void Test_configure_stdout() {
  LogOutput* stdoutput = LogOutput::Stdout;
  TestLogSavedConfig tlsc;

  // Enable 'logging=info', verifying it has been set
  LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(logging));
  assert_str_eq("logging=info", stdoutput->config_string());
  assert(log_is_enabled(Info, logging), "logging was not properly enabled");

  // Enable 'gc=debug' (no wildcard), verifying no other tags are enabled
  LogConfiguration::configure_stdout(LogLevel::Debug, true, LOG_TAGS(gc));
  // No '+' character means only single tags are enabled, and no combinations
  assert_char_not_in('+', stdoutput->config_string());
  assert(log_is_enabled(Debug, gc), "logging was not properly enabled");

  // Enable 'gc*=trace' (with wildcard), verifying at least one tag combination is enabled (gc+...)
  LogConfiguration::configure_stdout(LogLevel::Trace, false, LOG_TAGS(gc));
  assert_char_in('+', stdoutput->config_string());
  assert(log_is_enabled(Trace, gc), "logging was not properly enabled");

  // Disable 'gc*' and 'logging', verifying all logging is properly disabled
  LogConfiguration::configure_stdout(LogLevel::Off, false, LOG_TAGS(gc));
  LogConfiguration::configure_stdout(LogLevel::Off, true, LOG_TAGS(logging));
  assert_str_eq("all=off", stdoutput->config_string());
}

static size_t number_of_lines_with_substring_in_file(const char* filename,
                                                     const char* substr) {
  ResourceMark rm;
  size_t ret = 0;
  FILE* fp = fopen(filename, "r");
  assert(fp != NULL, "error opening file %s: %s", filename, strerror(errno));

  int buflen = 512;
  char* buf = NEW_RESOURCE_ARRAY(char, buflen);
  long pos = 0;

  while (fgets(buf, buflen, fp) != NULL) {
    if (buf[strlen(buf) - 1] != '\n' && !feof(fp)) {
      // retry with a larger buffer
      buf = REALLOC_RESOURCE_ARRAY(char, buf, buflen, buflen * 2);
      buflen *= 2;
      // rewind to beginning of line
      fseek(fp, pos, SEEK_SET);
      continue;
    }
    pos = ftell(fp);
    if (strstr(buf, substr) != NULL) {
      ret++;
    }
  }

  fclose(fp);
  return ret;
}

static bool file_exists(const char* filename) {
  struct stat st;
  return os::stat(filename, &st) == 0;
}

static void delete_file(const char* filename) {
  if (!file_exists(filename)) {
    return;
  }
  int ret = remove(filename);
  assert(ret == 0, "failed to remove file '%s': %s", filename, strerror(errno));
}

static void create_directory(const char* name) {
  assert(!file_exists(name), "can't create directory: %s already exists", name);
  bool failed;
#ifdef _WINDOWS
  failed = !CreateDirectory(name, NULL);
#else
  failed = mkdir(name, 0777);
#endif
  assert(!failed, "failed to create directory %s", name);
}

static const char* ExpectedLine = "a (hopefully) unique log line for testing";

static void init_file(const char* filename, const char* options = "") {
  LogConfiguration::parse_log_arguments(filename, "logging=trace", "", options,
                                        Log(logging)::error_stream());
  log_debug(logging)("%s", ExpectedLine);
  LogConfiguration::parse_log_arguments(filename, "all=off", "", "",
                                        Log(logging)::error_stream());
}

void Test_log_file_startup_rotation() {
  ResourceMark rm;
  const size_t rotations = 5;
  const char* filename = "start-rotate-test";
  char* rotated_file[rotations];
  for (size_t i = 0; i < rotations; i++) {
    size_t len = strlen(filename) + 3;
    rotated_file[i] = NEW_RESOURCE_ARRAY(char, len);
    jio_snprintf(rotated_file[i], len, "%s." SIZE_FORMAT, filename, i);
    delete_file(rotated_file[i]);
  };

  delete_file(filename);
  init_file(filename);
  assert(file_exists(filename),
         "configured logging to file '%s' but file was not found", filename);

  // Initialize the same file a bunch more times to trigger rotations
  for (size_t i = 0; i < rotations; i++) {
    init_file(filename);
    assert(file_exists(rotated_file[i]), "existing file was not rotated");
  }

  // Remove a file and expect its slot to be re-used
  delete_file(rotated_file[1]);
  init_file(filename);
  assert(file_exists(rotated_file[1]), "log file not properly rotated");

  // Clean up after test
  delete_file(filename);
  for (size_t i = 0; i < rotations; i++) {
    delete_file(rotated_file[i]);
  }
}

void Test_log_file_startup_truncation() {
  ResourceMark rm;
  const char* filename = "start-truncate-test";
  const char* archived_filename = "start-truncate-test.0";

  delete_file(filename);
  delete_file(archived_filename);

  // Use the same log file twice and expect it to be overwritten/truncated
  init_file(filename, "filecount=0");
  assert(file_exists(filename), "couldn't find log file: %s", filename);

  init_file(filename, "filecount=0");
  assert(file_exists(filename), "couldn't find log file: %s", filename);
  assert(!file_exists(archived_filename),
         "existing log file %s was not properly truncated when filecount was 0",
         filename);

  // Verify that the file was really truncated and not just appended
  assert(number_of_lines_with_substring_in_file(filename, ExpectedLine) == 1,
         "log file %s appended rather than truncated", filename);

  delete_file(filename);
  delete_file(archived_filename);
}

static int Test_logconfiguration_subscribe_triggered = 0;

static void Test_logconfiguration_subscribe_helper() {
  Test_logconfiguration_subscribe_triggered++;
}

void Test_logconfiguration_subscribe() {
  ResourceMark rm;
  Log(logging) log;

  TestLogSavedConfig log_cfg("stdout", "logging*=trace");

  LogConfiguration::register_update_listener(&Test_logconfiguration_subscribe_helper);

  LogConfiguration::parse_log_arguments("stdout", "logging=trace", NULL, NULL, log.error_stream());
  assert(Test_logconfiguration_subscribe_triggered == 1, "subscription not triggered (1)");

  LogConfiguration::configure_stdout(LogLevel::Debug, true, LOG_TAGS(gc));
  assert(Test_logconfiguration_subscribe_triggered == 2, "subscription not triggered (2)");

  LogConfiguration::disable_logging();
  assert(Test_logconfiguration_subscribe_triggered == 3, "subscription not triggered (3)");

  // We need to renable stderr error logging since "disable_logging" disable it all.
  // TestLogSavedConfig log_cfg will only renable stdout for us.
  LogConfiguration::parse_log_arguments("stderr", "all=warning", NULL, NULL, log.error_stream());
  assert(Test_logconfiguration_subscribe_triggered == 4, "subscription not triggered (3)");
}

#define LOG_PREFIX_STR "THE_PREFIX "
#define LOG_LINE_STR "a log line"

size_t Test_log_prefix_prefixer(char* buf, size_t len) {
  int ret = jio_snprintf(buf, len, LOG_PREFIX_STR);
  assert(ret > 0, "Failed to print prefix. Log buffer too small?");
  return (size_t) ret;
}

void Test_log_prefix() {
  TestLogFile log_file("log_prefix");
  TestLogSavedConfig log_cfg(log_file.name(), "logging+test=trace");

  log_trace(logging, test)(LOG_LINE_STR);

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");
  char output[1024];
  if (fgets(output, 1024, fp) != NULL) {
    assert(strstr(output, LOG_PREFIX_STR LOG_LINE_STR), "logging prefix error");
  }
  fclose(fp);
}

void Test_log_big() {
  char big_msg[4096] = {0};
  char Xchar = '~';

  TestLogFile log_file("log_big");
  TestLogSavedConfig log_cfg(log_file.name(), "logging+test=trace");

  memset(big_msg, Xchar, sizeof(big_msg) - 1);

  log_trace(logging, test)("%s", big_msg);

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");
  char output[sizeof(big_msg)+128 /*decorators*/ ];
  if (fgets(output, sizeof(output), fp) != NULL) {
    assert(strstr(output, LOG_PREFIX_STR), "logging prefix error");
    size_t count = 0;
    for (size_t ps = 0 ; output[ps + count] != '\0'; output[ps + count] == Xchar ? count++ : ps++);
    assert(count == (sizeof(big_msg) - 1) , "logging msg error");
  }
  fclose(fp);
}

void Test_logtagset_duplicates() {
  for (LogTagSet* ts = LogTagSet::first(); ts != NULL; ts = ts->next()) {
    char ts_name[512];
    ts->label(ts_name, sizeof(ts_name), ",");

    // verify that NO_TAG is never followed by a real tag
    for (size_t i = 0; i < LogTag::MaxTags; i++) {
      if (ts->tag(i) == LogTag::__NO_TAG) {
        for (i++; i < LogTag::MaxTags; i++) {
          assert(ts->tag(i) == LogTag::__NO_TAG,
                 "NO_TAG was followed by a real tag (%s) in tagset %s",
                 LogTag::name(ts->tag(i)), ts_name);
        }
      }
    }

    // verify that there are no duplicate tagsets (same tags in different order)
    for (LogTagSet* other = ts->next(); other != NULL; other = other->next()) {
      if (ts->ntags() != other->ntags()) {
        continue;
      }
      bool equal = true;
      for (size_t i = 0; i < ts->ntags(); i++) {
        LogTagType tag = ts->tag(i);
        if (!other->contains(tag)) {
          equal = false;
          break;
        }
      }
      // Since tagsets are implemented using template arguments, using both of
      // the (logically equivalent) tagsets (t1, t2) and (t2, t1) somewhere will
      // instantiate two different LogTagSetMappings. This causes multiple
      // tagset instances to be created for the same logical set. We want to
      // avoid this to save time, memory and prevent any confusion around it.
      if (equal) {
        char other_name[512];
        other->label(other_name, sizeof(other_name), ",");
        assert(false, "duplicate LogTagSets found: '%s' vs '%s' "
               "(tags must always be specified in the same order for each tagset)",
               ts_name, other_name);
      }
    }
  }
}

#define Test_logtarget_string_literal "First line"


static void Test_logtarget_on() {
  TestLogFile log_file("log_target");
  TestLogSavedConfig tlsc(log_file.name(), "gc=debug");

  LogTarget(Debug, gc) log;

  assert(log.is_enabled(), "assert");

  // Log the line and expect it to be available in the output file.
  log.print(Test_logtarget_string_literal);

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp != NULL, "File read error");

  char output[256 /* Large enough buffer */];
  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  assert(strstr(output, Test_logtarget_string_literal) != NULL, "log line missing");

  fclose(fp);
}

static void Test_logtarget_off() {
  TestLogFile log_file("log_target");
  TestLogSavedConfig tlsc(log_file.name(), "gc=info");

  LogTarget(Debug, gc) log;

  if (log.is_enabled()) {
    // The log config could have been redirected gc=debug to a file. If gc=debug
    // is enabled, we can only test that the LogTarget returns the same value
    // as the log_is_enabled function. The rest of the test will be ignored.
    assert(log.is_enabled() == log_is_enabled(Debug, gc), "assert");
    log_warning(logging)("This test doesn't support runs with -Xlog");
    return;
  }

  // Try to log, but expect this to be filtered out.
  log.print(Test_logtarget_string_literal);

  // Log a dummy line so that fgets doesn't return NULL because the file is empty.
  log_info(gc)("Dummy line");

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp != NULL, "File read error");

  char output[256 /* Large enough buffer */];
  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  assert(strstr(output, Test_logtarget_string_literal) == NULL, "log line not missing");

  fclose(fp);
}

void Test_logtarget() {
  Test_logtarget_on();
  Test_logtarget_off();
}


static void Test_logstream_helper(outputStream* stream) {
  TestLogFile log_file("log_stream");
  TestLogSavedConfig tlsc(log_file.name(), "gc=debug");

  // Try to log, but expect this to be filtered out.
  stream->print("%d ", 3); stream->print("workers"); stream->cr();

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp != NULL, "File read error");

  char output[256 /* Large enough buffer */];
  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  assert(strstr(output, "3 workers") != NULL, "log line missing");

  fclose(fp);
}

static void Test_logstream_log() {
  Log(gc) log;
  LogStream stream(log.debug());

  Test_logstream_helper(&stream);
}

static void Test_logstream_logtarget() {
  LogTarget(Debug, gc) log;
  LogStream stream(log);

  Test_logstream_helper(&stream);
}

static void Test_logstream_logstreamhandle() {
  LogStreamHandle(Debug, gc) stream;

  Test_logstream_helper(&stream);
}

static void Test_logstream_no_rm() {
  ResourceMark rm;
  outputStream* stream = LogTarget(Debug, gc)::stream();

  Test_logstream_helper(stream);
}

static void Test_logstreamcheap_log() {
  Log(gc) log;
  LogStreamCHeap stream(log.debug());

  Test_logstream_helper(&stream);
}

static void Test_logstreamcheap_logtarget() {
  LogTarget(Debug, gc) log;
  LogStreamCHeap stream(log);

  Test_logstream_helper(&stream);
}

void Test_logstream() {
  // Test LogStreams with embedded ResourceMark.
  Test_logstream_log();
  Test_logstream_logtarget();
  Test_logstream_logstreamhandle();

  // Test LogStreams without embedded ResourceMark.
  Test_logstream_no_rm();

  // Test LogStreams backed by CHeap memory.
  Test_logstreamcheap_log();
  Test_logstreamcheap_logtarget();
}

void Test_loghandle_on() {
  TestLogFile log_file("log_handle");
  TestLogSavedConfig tlsc(log_file.name(), "gc=debug");

  Log(gc) log;
  LogHandle log_handle(log);

  assert(log_handle.is_debug(), "assert");

  // Try to log through a LogHandle.
  log_handle.debug("%d workers", 3);

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");

  char output[256 /* Large enough buffer */];
  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  assert(strstr(output, "3 workers") != NULL, "log line missing");

  fclose(fp);
}

void Test_loghandle_off() {
  TestLogFile log_file("log_handle");
  TestLogSavedConfig tlsc(log_file.name(), "gc=info");

  Log(gc) log;
  LogHandle log_handle(log);

  if (log_handle.is_debug()) {
    // The log config could have been redirected gc=debug to a file. If gc=debug
    // is enabled, we can only test that the LogTarget returns the same value
    // as the log_is_enabled function. The rest of the test will be ignored.
    assert(log_handle.is_debug() == log_is_enabled(Debug, gc), "assert");
    log_warning(logging)("This test doesn't support runs with -Xlog");
    return;
  }

  // Try to log through a LogHandle. Should fail, since only info is turned on.
  log_handle.debug("%d workers", 3);

  // Log a dummy line so that fgets doesn't return NULL because the file is empty.
  log_info(gc)("Dummy line");

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");

  char output[256 /* Large enough buffer */];
  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  assert(strstr(output, "3 workers") == NULL, "log line missing");

  fclose(fp);
}

void Test_loghandle() {
  Test_loghandle_on();
  Test_loghandle_off();
}

static void Test_logtargethandle_on() {
  TestLogFile log_file("log_handle");
  TestLogSavedConfig tlsc(log_file.name(), "gc=debug");

  LogTarget(Debug, gc) log;
  LogTargetHandle log_handle(log);

  assert(log_handle.is_enabled(), "assert");

  // Try to log through a LogHandle.
  log_handle.print("%d workers", 3);

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");

  char output[256 /* Large enough buffer */];
  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  assert(strstr(output, "3 workers") != NULL, "log line missing");

  fclose(fp);
}

static void Test_logtargethandle_off() {
  TestLogFile log_file("log_handle");
  TestLogSavedConfig tlsc(log_file.name(), "gc=info");

  LogTarget(Debug, gc) log;
  LogTargetHandle log_handle(log);

  if (log_handle.is_enabled()) {
    // The log config could have been redirected gc=debug to a file. If gc=debug
    // is enabled, we can only test that the LogTarget returns the same value
    // as the log_is_enabled function. The rest of the test will be ignored.
    assert(log_handle.is_enabled() == log_is_enabled(Debug, gc), "assert");
    log_warning(logging)("This test doesn't support runs with -Xlog");
    return;
  }

  // Try to log through a LogHandle. Should fail, since only info is turned on.
  log_handle.print("%d workers", 3);

  // Log a dummy line so that fgets doesn't return NULL because the file is empty.
  log_info(gc)("Dummy line");

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");

  char output[256 /* Large enough buffer */];
  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  assert(strstr(output, "3 workers") == NULL, "log line missing");

  fclose(fp);
}

void Test_logtargethandle() {
  Test_logtargethandle_on();
  Test_logtargethandle_off();
}

static void Test_log_gctracetime_full() {
  TestLogFile log_file("log_gctracetime");
  TestLogSavedConfig tlsc(log_file.name(), "gc=debug,gc+start=debug");

  LogTarget(Debug, gc) gc_debug;
  LogTarget(Debug, gc, start) gc_start_debug;

  assert(gc_debug.is_enabled(), "assert");
  assert(gc_start_debug.is_enabled(), "assert");

  {
    MutexLocker lock(Heap_lock); // Needed to read heap usage
    GCTraceTime(Debug, gc) timer("Test GC", NULL, GCCause::_allocation_failure, true);
  }

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");

  char output[256 /* Large enough buffer */];

  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc,start] Test GC (Allocation Failure) (2.975s)
  assert(strstr(output, "[gc,start") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC (Allocation Failure) (") != NULL, "Incorrect log line");
  assert(strstr(output, "s)") != NULL, "Incorrect log line");

  res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc      ] Test GC (Allocation Failure) 59M->59M(502M) (2.975s, 2.975s) 0.026ms
  assert(strstr(output, "[gc ") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC (Allocation Failure) ") != NULL, "Incorrect log line");
  assert(strstr(output, "M) (") != NULL, "Incorrect log line");
  assert(strstr(output, "s, ") != NULL, "Incorrect log line");
  assert(strstr(output, "s) ") != NULL, "Incorrect log line");
  assert(strstr(output, "ms") != NULL, "Incorrect log line");

  fclose(fp);
}

static void Test_log_gctracetime_full_multitag() {
  TestLogFile log_file("log_gctracetime");
  TestLogSavedConfig tlsc(log_file.name(), "gc+ref=debug,gc+ref+start=debug");

  LogTarget(Debug, gc, ref) gc_debug;
  LogTarget(Debug, gc, ref, start) gc_start_debug;

  assert(gc_debug.is_enabled(), "assert");
  assert(gc_start_debug.is_enabled(), "assert");

  {
    MutexLocker lock(Heap_lock); // Needed to read heap usage
    GCTraceTime(Debug, gc, ref) timer("Test GC", NULL, GCCause::_allocation_failure, true);
  }

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");

  char output[256 /* Large enough buffer */];

  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc,start] Test GC (Allocation Failure) (2.975s)
  assert(strstr(output, "[gc,ref,start") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC (Allocation Failure) (") != NULL, "Incorrect log line");
  assert(strstr(output, "s)") != NULL, "Incorrect log line");

  res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc      ] Test GC (Allocation Failure) 59M->59M(502M) (2.975s, 2.975s) 0.026ms
  assert(strstr(output, "[gc,ref ") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC (Allocation Failure) ") != NULL, "Incorrect log line");
  assert(strstr(output, "M) (") != NULL, "Incorrect log line");
  assert(strstr(output, "s, ") != NULL, "Incorrect log line");
  assert(strstr(output, "s) ") != NULL, "Incorrect log line");
  assert(strstr(output, "ms") != NULL, "Incorrect log line");

  fclose(fp);
}

static void Test_log_gctracetime_no_heap() {
  TestLogFile log_file("log_gctracetime");
  TestLogSavedConfig tlsc(log_file.name(), "gc=debug,gc+start=debug");

  LogTarget(Debug, gc) gc_debug;
  LogTarget(Debug, gc, start) gc_start_debug;

  assert(gc_debug.is_enabled(), "assert");
  assert(gc_start_debug.is_enabled(), "assert");

  {
    GCTraceTime(Debug, gc) timer("Test GC", NULL, GCCause::_allocation_failure, false);
  }

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");

  char output[256 /* Large enough buffer */];

  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc,start] Test GC (Allocation Failure) (2.975s)
  assert(strstr(output, "[gc,start") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC (Allocation Failure) (") != NULL, "Incorrect log line");
  assert(strstr(output, "s)") != NULL, "Incorrect log line");

  res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc      ] Test GC (Allocation Failure) (2.975s, 2.975s) 0.026ms
  assert(strstr(output, "[gc ") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC (Allocation Failure) (") != NULL, "Incorrect log line");
  assert(strstr(output, "M) (") == NULL, "Incorrect log line");
  assert(strstr(output, "s, ") != NULL, "Incorrect log line");
  assert(strstr(output, "s) ") != NULL, "Incorrect log line");
  assert(strstr(output, "ms") != NULL, "Incorrect log line");

  fclose(fp);
}

static void Test_log_gctracetime_no_cause() {
  TestLogFile log_file("log_gctracetime");
  TestLogSavedConfig tlsc(log_file.name(), "gc=debug,gc+start=debug");

  LogTarget(Debug, gc) gc_debug;
  LogTarget(Debug, gc, start) gc_start_debug;

  assert(gc_debug.is_enabled(), "assert");
  assert(gc_start_debug.is_enabled(), "assert");

  {
    MutexLocker lock(Heap_lock); // Needed to read heap usage
    GCTraceTime(Debug, gc) timer("Test GC", NULL, GCCause::_no_gc, true);
  }

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");

  char output[256 /* Large enough buffer */];

  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc,start] Test GC (2.975s)
  assert(strstr(output, "[gc,start") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC (") != NULL, "Incorrect log line");
  assert(strstr(output, "s)") != NULL, "Incorrect log line");

  res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc      ] Test GC 59M->59M(502M) (2.975s, 2.975s) 0.026ms
  assert(strstr(output, "[gc ") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC ") != NULL, "Incorrect log line");
  assert(strstr(output, "M) (") != NULL, "Incorrect log line");
  assert(strstr(output, "s, ") != NULL, "Incorrect log line");
  assert(strstr(output, "s) ") != NULL, "Incorrect log line");
  assert(strstr(output, "ms") != NULL, "Incorrect log line");

  fclose(fp);
}

static void Test_log_gctracetime_no_heap_no_cause() {
  TestLogFile log_file("log_gctracetime");
  TestLogSavedConfig tlsc(log_file.name(), "gc=debug,gc+start=debug");

  LogTarget(Debug, gc) gc_debug;
  LogTarget(Debug, gc, start) gc_start_debug;

  assert(gc_debug.is_enabled(), "assert");
  assert(gc_start_debug.is_enabled(), "assert");

  {
    MutexLocker lock(Heap_lock); // Needed to read heap usage
    GCTraceTime(Debug, gc) timer("Test GC", NULL, GCCause::_no_gc, false);
  }

  FILE* fp = fopen(log_file.name(), "r");
  assert(fp, "File read error");

  char output[256 /* Large enough buffer */];

  char* res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc,start] Test GC (2.975s)
  assert(strstr(output, "[gc,start") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC (") != NULL, "Incorrect log line");
  assert(strstr(output, "s)") != NULL, "Incorrect log line");

  res = fgets(output, sizeof(output), fp);
  assert(res != NULL, "assert");

  // [2.975s][debug][gc      ] Test GC (2.975s, 2.975s) 0.026ms
  assert(strstr(output, "[gc ") != NULL, "Incorrect tag set");
  assert(strstr(output, "] Test GC (") != NULL, "Incorrect log line");
  assert(strstr(output, "M) (") == NULL, "Incorrect log line");
  assert(strstr(output, "s, ") != NULL, "Incorrect log line");
  assert(strstr(output, "s) ") != NULL, "Incorrect log line");
  assert(strstr(output, "ms") != NULL, "Incorrect log line");

  fclose(fp);
}

void Test_log_gctracetime() {
  Test_log_gctracetime_full();
  Test_log_gctracetime_full_multitag();
  Test_log_gctracetime_no_heap();
  Test_log_gctracetime_no_cause();
  Test_log_gctracetime_no_heap_no_cause();
}

void Test_invalid_log_file() {
  ResourceMark rm;
  stringStream ss;
  const char* target_name = "tmplogdir";

  // Attempt to log to a directory (existing log not a regular file)
  create_directory(target_name);
  LogFileOutput bad_file("tmplogdir");
  assert(bad_file.initialize("", &ss) == false, "file was initialized "
         "when there was an existing directory with the same name");
  assert(strstr(ss.as_string(), "tmplogdir is not a regular file") != NULL,
         "missing expected error message, received msg: %s", ss.as_string());
  ss.reset();
  remove(target_name);
}

#endif // PRODUCT
