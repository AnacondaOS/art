/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef ART_CMDLINE_CMDLINE_TYPES_H_
#define ART_CMDLINE_CMDLINE_TYPES_H_

#define CMDLINE_NDEBUG 1  // Do not output any debugging information for parsing.

#include <cstdint>
#include <list>
#include <ostream>

#include "android-base/parsebool.h"
#include "android-base/stringprintf.h"
#include "cmdline_type_parser.h"
#include "detail/cmdline_debug_detail.h"
#include "memory_representation.h"

#include "android-base/logging.h"
#include "android-base/strings.h"

// Includes for the types that are being specialized
#include <string>
#include "base/time_utils.h"
#include "base/logging.h"
#include "experimental_flags.h"
#include "gc/collector_type.h"
#include "gc/space/large_object_space.h"
#include "jdwp_provider.h"
#include "jit/profile_saver_options.h"
#include "plugin.h"
#include "read_barrier_config.h"
#include "ti/agent.h"
#include "unit.h"

namespace art {

// The default specialization will always fail parsing the type from a string.
// Provide your own specialization that inherits from CmdlineTypeParser<T>
// and implements either Parse or ParseAndAppend
// (only if the argument was defined with ::AppendValues()) but not both.
template <typename T>
struct CmdlineType : CmdlineTypeParser<T> {
};

// Specializations for CmdlineType<T> follow:

// Parse argument definitions for Unit-typed arguments.
template <>
struct CmdlineType<Unit> : CmdlineTypeParser<Unit> {
  Result Parse(const std::string& args) {
    if (args == "") {
      return Result::Success(Unit{});
    }
    return Result::Failure("Unexpected extra characters " + args);
  }
};

template <>
struct CmdlineType<bool> : CmdlineTypeParser<bool> {
  Result Parse(const std::string& args) {
    switch (::android::base::ParseBool(args)) {
      case ::android::base::ParseBoolResult::kError:
        return Result::Failure("Could not parse '" + args + "' as boolean");
      case ::android::base::ParseBoolResult::kTrue:
        return Result::Success(true);
      case ::android::base::ParseBoolResult::kFalse:
        return Result::Success(false);
    }
  }

  static const char* DescribeType() { return "true|false|1|0|y|n|yes|no|on|off"; }
};

template <>
struct CmdlineType<JdwpProvider> : CmdlineTypeParser<JdwpProvider> {
  /*
   * Handle a single JDWP provider name. Must be either 'internal', 'default', or the file name of
   * an agent. A plugin will make use of this and the jdwpOptions to set up jdwp when appropriate.
   */
  Result Parse(const std::string& option) {
    if (option == "help") {
      return Result::Usage(
          "Example: -XjdwpProvider:none to disable JDWP\n"
          "Example: -XjdwpProvider:adbconnection for adb connection mediated jdwp implementation\n"
          "Example: -XjdwpProvider:default for the default jdwp implementation\n");
    } else if (option == "default") {
      return Result::Success(JdwpProvider::kDefaultJdwpProvider);
    } else if (option == "adbconnection") {
      return Result::Success(JdwpProvider::kAdbConnection);
    } else if (option == "none") {
      return Result::Success(JdwpProvider::kNone);
    } else {
      return Result::Failure(std::string("not a valid jdwp provider: ") + option);
    }
  }
  static const char* Name() { return "JdwpProvider"; }
  static const char* DescribeType() { return "none|adbconnection|default"; }
};

template <size_t Divisor>
struct CmdlineType<Memory<Divisor>> : CmdlineTypeParser<Memory<Divisor>> {
  using typename CmdlineTypeParser<Memory<Divisor>>::Result;

  Result Parse(const std::string& arg) {
    CMDLINE_DEBUG_LOG << "Parsing memory: " << arg << std::endl;
    size_t val = ParseMemoryOption(arg.c_str(), Divisor);
    CMDLINE_DEBUG_LOG << "Memory parsed to size_t value: " << val << std::endl;

    if (val == 0) {
      return Result::Failure(std::string("not a valid memory value, or not divisible by ")
                             + std::to_string(Divisor));
    }

    return Result::Success(Memory<Divisor>(val));
  }

  // Parse a string of the form /[0-9]+[kKmMgG]?/, which is used to specify
  // memory sizes.  [kK] indicates kilobytes, [mM] megabytes, and
  // [gG] gigabytes.
  //
  // "s" should point just past the "-Xm?" part of the string.
  // "div" specifies a divisor, e.g. 1024 if the value must be a multiple
  // of 1024.
  //
  // The spec says the -Xmx and -Xms options must be multiples of 1024.  It
  // doesn't say anything about -Xss.
  //
  // Returns 0 (a useless size) if "s" is malformed or specifies a low or
  // non-evenly-divisible value.
  //
  static size_t ParseMemoryOption(const char* s, size_t div) {
    // strtoul accepts a leading [+-], which we don't want,
    // so make sure our string starts with a decimal digit.
    if (isdigit(*s)) {
      char* s2;
      size_t val = strtoul(s, &s2, 10);
      if (s2 != s) {
        // s2 should be pointing just after the number.
        // If this is the end of the string, the user
        // has specified a number of bytes.  Otherwise,
        // there should be exactly one more character
        // that specifies a multiplier.
        if (*s2 != '\0') {
          // The remainder of the string is either a single multiplier
          // character, or nothing to indicate that the value is in
          // bytes.
          char c = *s2++;
          if (*s2 == '\0') {
            size_t mul;
            if (c == '\0') {
              mul = 1;
            } else if (c == 'k' || c == 'K') {
              mul = KB;
            } else if (c == 'm' || c == 'M') {
              mul = MB;
            } else if (c == 'g' || c == 'G') {
              mul = GB;
            } else {
              // Unknown multiplier character.
              return 0;
            }

            if (val <= std::numeric_limits<size_t>::max() / mul) {
              val *= mul;
            } else {
              // Clamp to a multiple of 1024.
              val = std::numeric_limits<size_t>::max() & ~(1024-1);
            }
          } else {
            // There's more than one character after the numeric part.
            return 0;
          }
        }
        // The man page says that a -Xm value must be a multiple of 1024.
        if (val % div == 0) {
          return val;
        }
      }
    }
    return 0;
  }

  static const char* Name() { return Memory<Divisor>::Name(); }
  static const char* DescribeType() {
    static std::string str;
    if (str.empty()) {
      str = "Memory with granularity of " + std::to_string(Divisor) + " bytes";
    }
    return str.c_str();
  }
};

template <>
struct CmdlineType<double> : CmdlineTypeParser<double> {
  Result Parse(const std::string& str) {
    char* end = nullptr;
    errno = 0;
    double value = strtod(str.c_str(), &end);

    if (*end != '\0') {
      return Result::Failure("Failed to parse double from " + str);
    }
    if (errno == ERANGE) {
      return Result::OutOfRange(
          "Failed to parse double from " + str + "; overflow/underflow occurred");
    }

    return Result::Success(value);
  }

  static const char* Name() { return "double"; }
  static const char* DescribeType() { return "double value"; }
};

template <typename T>
static inline CmdlineParseResult<T> ParseNumeric(const std::string& str) {
  static_assert(sizeof(T) < sizeof(long long int),  // NOLINT [runtime/int] [4]
                "Current support is restricted.");

  const char* begin = str.c_str();
  char* end;

  // Parse into a larger type (long long) because we can't use strtoul
  // since it silently converts negative values into unsigned long and doesn't set errno.
  errno = 0;
  long long int result = strtoll(begin, &end, 10);  // NOLINT [runtime/int] [4]
  if (begin == end || *end != '\0' || errno == EINVAL) {
    return CmdlineParseResult<T>::Failure("Failed to parse integer from " + str);
  } else if ((errno == ERANGE) ||  // NOLINT [runtime/int] [4]
      result < std::numeric_limits<T>::min() || result > std::numeric_limits<T>::max()) {
    return CmdlineParseResult<T>::OutOfRange(
        "Failed to parse integer from " + str + "; out of range");
  }

  return CmdlineParseResult<T>::Success(static_cast<T>(result));
}

template <>
struct CmdlineType<unsigned int> : CmdlineTypeParser<unsigned int> {
  Result Parse(const std::string& str) {
    return ParseNumeric<unsigned int>(str);
  }

  static const char* Name() { return "unsigned integer"; }
  static const char* DescribeType() { return "unsigned integer value"; }
};

template <>
struct CmdlineType<uint16_t> : CmdlineTypeParser<uint16_t> {
  Result Parse(const std::string& str) {
    return ParseNumeric<uint16_t>(str);
  }

  static const char* Name() { return "unsigned 16-bit integer"; }
  static const char* DescribeType() { return "unsigned 16-bit integer value"; }
};


template <>
struct CmdlineType<int> : CmdlineTypeParser<int> {
  Result Parse(const std::string& str) {
    return ParseNumeric<int>(str);
  }

  static const char* Name() { return "integer"; }
  static const char* DescribeType() { return "integer value"; }
};

// Lightweight nanosecond value type. Allows parser to convert user-input from milliseconds
// to nanoseconds automatically after parsing.
//
// All implicit conversion from uint64_t uses nanoseconds.
struct MillisecondsToNanoseconds {
  // Create from nanoseconds.
  MillisecondsToNanoseconds(uint64_t nanoseconds) : nanoseconds_(nanoseconds) {  // NOLINT [runtime/explicit] [5]
  }

  // Create from milliseconds.
  static MillisecondsToNanoseconds FromMilliseconds(unsigned int milliseconds) {
    return MillisecondsToNanoseconds(MsToNs(milliseconds));
  }

  // Get the underlying nanoseconds value.
  uint64_t GetNanoseconds() const {
    return nanoseconds_;
  }

  // Get the milliseconds value [via a conversion]. Loss of precision will occur.
  uint64_t GetMilliseconds() const {
    return NsToMs(nanoseconds_);
  }

  // Get the underlying nanoseconds value.
  operator uint64_t() const {
    return GetNanoseconds();
  }

  // Default constructors/copy-constructors.
  MillisecondsToNanoseconds() : nanoseconds_(0ul) {}
  MillisecondsToNanoseconds(const MillisecondsToNanoseconds&) = default;
  MillisecondsToNanoseconds(MillisecondsToNanoseconds&&) = default;

 private:
  uint64_t nanoseconds_;
};

template <>
struct CmdlineType<MillisecondsToNanoseconds> : CmdlineTypeParser<MillisecondsToNanoseconds> {
  Result Parse(const std::string& str) {
    CmdlineType<unsigned int> uint_parser;
    CmdlineParseResult<unsigned int> res = uint_parser.Parse(str);

    if (res.IsSuccess()) {
      return Result::Success(MillisecondsToNanoseconds::FromMilliseconds(res.GetValue()));
    } else {
      return Result::CastError(res);
    }
  }

  static const char* Name() { return "MillisecondsToNanoseconds"; }
  static const char* DescribeType() { return "millisecond value"; }
};

template <>
struct CmdlineType<std::string> : CmdlineTypeParser<std::string> {
  Result Parse(const std::string& args) {
    return Result::Success(args);
  }

  Result ParseAndAppend(const std::string& args,
                        std::string& existing_value) {
    if (existing_value.empty()) {
      existing_value = args;
    } else {
      existing_value += ' ';
      existing_value += args;
    }
    return Result::SuccessNoValue();
  }
  static const char* DescribeType() { return "string value"; }
};

template <>
struct CmdlineType<std::vector<Plugin>> : CmdlineTypeParser<std::vector<Plugin>> {
  Result Parse(const std::string& args) {
    assert(false && "Use AppendValues() for a Plugin vector type");
    return Result::Failure("Unconditional failure: Plugin vector must be appended: " + args);
  }

  Result ParseAndAppend(const std::string& args,
                        std::vector<Plugin>& existing_value) {
    existing_value.push_back(Plugin::Create(args));
    return Result::SuccessNoValue();
  }

  static const char* Name() { return "std::vector<Plugin>"; }
  static const char* DescribeType() { return "/path/to/libplugin.so"; }
};

template <>
struct CmdlineType<std::list<ti::AgentSpec>> : CmdlineTypeParser<std::list<ti::AgentSpec>> {
  Result Parse(const std::string& args) {
    assert(false && "Use AppendValues() for an Agent list type");
    return Result::Failure("Unconditional failure: Agent list must be appended: " + args);
  }

  Result ParseAndAppend(const std::string& args,
                        std::list<ti::AgentSpec>& existing_value) {
    existing_value.emplace_back(args);
    return Result::SuccessNoValue();
  }

  static const char* Name() { return "std::list<ti::AgentSpec>"; }
  static const char* DescribeType() { return "/path/to/libagent.so=options"; }
};

template <>
struct CmdlineType<std::vector<std::string>> : CmdlineTypeParser<std::vector<std::string>> {
  Result Parse(const std::string& args) {
    assert(false && "Use AppendValues() for a string vector type");
    return Result::Failure("Unconditional failure: string vector must be appended: " + args);
  }

  Result ParseAndAppend(const std::string& args,
                        std::vector<std::string>& existing_value) {
    existing_value.push_back(args);
    return Result::SuccessNoValue();
  }

  static const char* Name() { return "std::vector<std::string>"; }
  static const char* DescribeType() { return "string value"; }
};

template <>
struct CmdlineType<std::vector<int>> : CmdlineTypeParser<std::vector<int>> {
  Result Parse(const std::string& args) {
    assert(false && "Use AppendValues() for a int vector type");
    return Result::Failure("Unconditional failure: string vector must be appended: " + args);
  }

  Result ParseAndAppend(const std::string& args,
                        std::vector<int>& existing_value) {
    auto result = ParseNumeric<int>(args);
    if (result.IsSuccess()) {
      existing_value.push_back(result.GetValue());
    } else {
      return Result::CastError(result);
    }
    return Result::SuccessNoValue();
  }

  static const char* Name() { return "std::vector<int>"; }
  static const char* DescribeType() { return "int values"; }
};

template <typename ArgType, char Separator>
struct ParseList {
  explicit ParseList(std::vector<ArgType>&& list) : list_(list) {}

  operator std::vector<ArgType>() const {
    return list_;
  }

  operator std::vector<ArgType>&&() && {
    return std::move(list_);
  }

  size_t Size() const {
    return list_.size();
  }

  std::string Join() const {
    return android::base::Join(list_, Separator);
  }

  ParseList() = default;
  ParseList(const ParseList&) = default;
  ParseList(ParseList&&) noexcept = default;

 private:
  std::vector<ArgType> list_;
};

template <char Separator>
using ParseIntList = ParseList<int, Separator>;

template <char Separator>
struct ParseStringList : public ParseList<std::string, Separator> {
  explicit ParseStringList(std::vector<std::string>&& list) : ParseList<std::string, Separator>(std::move(list)) {}

  static ParseStringList<Separator> Split(const std::string& str) {
    std::vector<std::string> list;
    art::Split(str, Separator, &list);
    return ParseStringList<Separator>(std::move(list));
  }

  ParseStringList() = default;
  ParseStringList(const ParseStringList&) = default;
  ParseStringList(ParseStringList&&) noexcept = default;
};

template <char Separator>
struct CmdlineType<ParseStringList<Separator>> : CmdlineTypeParser<ParseStringList<Separator>> {
  using Result = CmdlineParseResult<ParseStringList<Separator>>;

  Result Parse(const std::string& args) {
    return Result::Success(ParseStringList<Separator>::Split(args));
  }

  static const char* Name() { return "ParseStringList<Separator>"; }
  static const char* DescribeType() {
    static std::string str;
    if (str.empty()) {
      str = android::base::StringPrintf("list separated by '%c'", Separator);
    }
    return str.c_str();
  }
};

template <char Separator>
struct CmdlineType<ParseIntList<Separator>> : CmdlineTypeParser<ParseIntList<Separator>> {
  using Result = CmdlineParseResult<ParseIntList<Separator>>;

  Result Parse(const std::string& args) {
    std::vector<int> list;
    const char* pos = args.c_str();
    errno = 0;

    while (true) {
      char* end = nullptr;
      int64_t value = strtol(pos, &end, 10);
      if (pos == end ||  errno == EINVAL) {
        return Result::Failure("Failed to parse integer from " + args);
      } else if ((errno == ERANGE) ||  // NOLINT [runtime/int] [4]
                 value < std::numeric_limits<int>::min() ||
                 value > std::numeric_limits<int>::max()) {
        return Result::OutOfRange("Failed to parse integer from " + args + "; out of range");
      }
      list.push_back(static_cast<int>(value));
      if (*end == '\0') {
        break;
      } else if (*end != Separator) {
        return Result::Failure(std::string("Unexpected character: ") + *end);
      }
      pos = end + 1;
    }
    return Result::Success(ParseIntList<Separator>(std::move(list)));
  }

  static const char* Name() { return "ParseIntList<Separator>"; }
  static const char* DescribeType() {
    static std::string str;
    if (str.empty()) {
      str = android::base::StringPrintf("integer list separated by '%c'", Separator);
    }
    return str.c_str();
  }
};

static gc::CollectorType ParseCollectorType(const std::string& option) {
  if (option == "MS" || option == "nonconcurrent") {
    return gc::kCollectorTypeMS;
  } else if (option == "CMS" || option == "concurrent") {
    return gc::kCollectorTypeCMS;
  } else if (option == "SS") {
    return gc::kCollectorTypeSS;
  } else if (option == "CC") {
    return gc::kCollectorTypeCC;
  } else if (option == "CMC") {
    return gc::kCollectorTypeCMC;
  } else {
    return gc::kCollectorTypeNone;
  }
}

struct XGcOption {
  // These defaults are used when the command line arguments for -Xgc:
  // are either omitted completely or partially.
  gc::CollectorType collector_type_ = gc::kCollectorTypeDefault;
  bool verify_pre_gc_heap_ = false;
  bool verify_pre_sweeping_heap_ = false;
  bool generational_gc = kEnableGenerationalGCByDefault;
  bool verify_post_gc_heap_ = false;
  bool verify_pre_gc_rosalloc_ = false;
  bool verify_pre_sweeping_rosalloc_ = false;
  bool verify_post_gc_rosalloc_ = false;
  // Do no measurements for kUseTableLookupReadBarrier to avoid test timeouts. b/31679493
  bool measure_ = kIsDebugBuild && !kUseTableLookupReadBarrier;
  bool gcstress_ = false;
};

template <>
struct CmdlineType<XGcOption> : CmdlineTypeParser<XGcOption> {
  Result Parse(const std::string& option) {  // -Xgc: already stripped
    XGcOption xgc{};
    // TODO: Deprecate and eventually remove -Xgc:[no]generational_cc option in
    // favor of -Xgc:[no]generational_gc.
    std::vector<std::string> gc_options;
    Split(option, ',', &gc_options);
    for (const std::string& gc_option : gc_options) {
      gc::CollectorType collector_type = ParseCollectorType(gc_option);
      if (collector_type != gc::kCollectorTypeNone) {
        xgc.collector_type_ = collector_type;
      } else if (gc_option == "preverify") {
        xgc.verify_pre_gc_heap_ = true;
      } else if (gc_option == "nopreverify") {
        xgc.verify_pre_gc_heap_ = false;
      }  else if (gc_option == "presweepingverify") {
        xgc.verify_pre_sweeping_heap_ = true;
      } else if (gc_option == "nopresweepingverify") {
        xgc.verify_pre_sweeping_heap_ = false;
      } else if (gc_option == "generational_cc" || gc_option == "generational_gc") {
        // Note: Option "-Xgc:generational_gc" can be passed directly by
        // app_process/zygote (see `android::AndroidRuntime::startVm`). If this
        // option is ever deprecated, it should still be accepted (but ignored)
        // for compatibility reasons (this should not prevent the runtime from
        // starting up).
        xgc.generational_gc = true;
      } else if (gc_option == "nogenerational_cc" || gc_option == "nogenerational_gc") {
        // Note: Option "-Xgc:nogenerational_gc" can be passed directly by
        // app_process/zygote (see `android::AndroidRuntime::startVm`). If this
        // option is ever deprecated, it should still be accepted (but ignored)
        // for compatibility reasons (this should not prevent the runtime from
        // starting up).
        xgc.generational_gc = false;
      } else if (gc_option == "postverify") {
        xgc.verify_post_gc_heap_ = true;
      } else if (gc_option == "nopostverify") {
        xgc.verify_post_gc_heap_ = false;
      } else if (gc_option == "preverify_rosalloc") {
        xgc.verify_pre_gc_rosalloc_ = true;
      } else if (gc_option == "nopreverify_rosalloc") {
        xgc.verify_pre_gc_rosalloc_ = false;
      } else if (gc_option == "presweepingverify_rosalloc") {
        xgc.verify_pre_sweeping_rosalloc_ = true;
      } else if (gc_option == "nopresweepingverify_rosalloc") {
        xgc.verify_pre_sweeping_rosalloc_ = false;
      } else if (gc_option == "postverify_rosalloc") {
        xgc.verify_post_gc_rosalloc_ = true;
      } else if (gc_option == "nopostverify_rosalloc") {
        xgc.verify_post_gc_rosalloc_ = false;
      } else if (gc_option == "gcstress") {
        xgc.gcstress_ = true;
      } else if (gc_option == "nogcstress") {
        xgc.gcstress_ = false;
      } else if (gc_option == "measure") {
        xgc.measure_ = true;
      } else if ((gc_option == "precise") ||
                 (gc_option == "noprecise") ||
                 (gc_option == "verifycardtable") ||
                 (gc_option == "noverifycardtable")) {
        // Ignored for backwards compatibility.
      } else {
        return Result::Usage(std::string("Unknown -Xgc option ") + gc_option);
      }
    }

    return Result::Success(std::move(xgc));
  }

  static const char* Name() { return "XgcOption"; }
  static const char* DescribeType() {
    return "MS|nonconccurent|concurrent|CMS|SS|CC|[no]preverify[_rosalloc]|"
           "[no]presweepingverify[_rosalloc]|[no]generation_cc|[no]postverify[_rosalloc]|"
           "[no]gcstress|measure|[no]precisce|[no]verifycardtable";
  }
};

struct BackgroundGcOption {
  // If background_collector_type_ is kCollectorTypeNone, it defaults to the
  // XGcOption::collector_type_ after parsing options. If you set this to
  // kCollectorTypeHSpaceCompact then we will do an hspace compaction when
  // we transition to background instead of a normal collector transition.
  gc::CollectorType background_collector_type_;

  BackgroundGcOption(gc::CollectorType background_collector_type)  // NOLINT [runtime/explicit] [5]
    : background_collector_type_(background_collector_type) {}
  BackgroundGcOption()
    : background_collector_type_(gc::kCollectorTypeNone) {
  }

  operator gc::CollectorType() const { return background_collector_type_; }
};

template<>
struct CmdlineType<BackgroundGcOption>
  : CmdlineTypeParser<BackgroundGcOption>, private BackgroundGcOption {
  Result Parse(const std::string& substring) {
    // Special handling for HSpaceCompact since this is only valid as a background GC type.
    if (substring == "HSpaceCompact") {
      background_collector_type_ = gc::kCollectorTypeHomogeneousSpaceCompact;
    } else {
      gc::CollectorType collector_type = ParseCollectorType(substring);
      if (collector_type != gc::kCollectorTypeNone) {
        background_collector_type_ = collector_type;
      } else {
        return Result::Failure();
      }
    }

    BackgroundGcOption res = *this;
    return Result::Success(res);
  }

  static const char* Name() { return "BackgroundGcOption"; }
  static const char* DescribeType() {
    return "HSpaceCompact|MS|nonconccurent|CMS|concurrent|SS|CC";
  }
};

template <>
struct CmdlineType<LogVerbosity> : CmdlineTypeParser<LogVerbosity> {
  Result Parse(const std::string& options) {
    LogVerbosity log_verbosity = LogVerbosity();

    std::vector<std::string> verbose_options;
    Split(options, ',', &verbose_options);
    for (size_t j = 0; j < verbose_options.size(); ++j) {
      if (verbose_options[j] == "class") {
        log_verbosity.class_linker = true;
      } else if (verbose_options[j] == "collector") {
        log_verbosity.collector = true;
      } else if (verbose_options[j] == "compiler") {
        log_verbosity.compiler = true;
      } else if (verbose_options[j] == "deopt") {
        log_verbosity.deopt = true;
      } else if (verbose_options[j] == "gc") {
        log_verbosity.gc = true;
      } else if (verbose_options[j] == "heap") {
        log_verbosity.heap = true;
      } else if (verbose_options[j] == "interpreter") {
        log_verbosity.interpreter = true;
      } else if (verbose_options[j] == "jdwp") {
        log_verbosity.jdwp = true;
      } else if (verbose_options[j] == "jit") {
        log_verbosity.jit = true;
      } else if (verbose_options[j] == "jni") {
        log_verbosity.jni = true;
      } else if (verbose_options[j] == "monitor") {
        log_verbosity.monitor = true;
      } else if (verbose_options[j] == "oat") {
        log_verbosity.oat = true;
      } else if (verbose_options[j] == "profiler") {
        log_verbosity.profiler = true;
      } else if (verbose_options[j] == "signals") {
        log_verbosity.signals = true;
      } else if (verbose_options[j] == "simulator") {
        log_verbosity.simulator = true;
      } else if (verbose_options[j] == "startup") {
        log_verbosity.startup = true;
      } else if (verbose_options[j] == "third-party-jni") {
        log_verbosity.third_party_jni = true;
      } else if (verbose_options[j] == "threads") {
        log_verbosity.threads = true;
      } else if (verbose_options[j] == "verifier") {
        log_verbosity.verifier = true;
      } else if (verbose_options[j] == "verifier-debug") {
        log_verbosity.verifier_debug = true;
      } else if (verbose_options[j] == "image") {
        log_verbosity.image = true;
      } else if (verbose_options[j] == "systrace-locks") {
        log_verbosity.systrace_lock_logging = true;
      } else if (verbose_options[j] == "plugin") {
        log_verbosity.plugin = true;
      } else if (verbose_options[j] == "agents") {
        log_verbosity.agents = true;
      } else if (verbose_options[j] == "dex") {
        log_verbosity.dex = true;
      } else {
        return Result::Usage(std::string("Unknown -verbose option ") + verbose_options[j]);
      }
    }

    return Result::Success(log_verbosity);
  }

  static const char* Name() { return "LogVerbosity"; }
  static const char* DescribeType() {
    return "class|collector|compiler|deopt|gc|heap|interpreter|jdwp|jit|jni|monitor|oat|profiler|"
           "signals|simulator|startup|third-party-jni|threads|verifier|verifier-debug|image|"
           "systrace-locks|plugin|agents|dex";
  }
};

template <>
struct CmdlineType<ProfileSaverOptions> : CmdlineTypeParser<ProfileSaverOptions> {
  using Result = CmdlineParseResult<ProfileSaverOptions>;

 private:
  using StringResult = CmdlineParseResult<std::string>;
  using DoubleResult = CmdlineParseResult<double>;

  template <typename T>
  static Result ParseInto(ProfileSaverOptions& options,
                          T ProfileSaverOptions::*pField,
                          CmdlineParseResult<T>&& result) {
    assert(pField != nullptr);

    if (result.IsSuccess()) {
      options.*pField = result.ReleaseValue();
      return Result::SuccessNoValue();
    }

    return Result::CastError(result);
  }

  static std::string RemovePrefix(const std::string& source) {
    size_t prefix_idx = source.find(':');

    if (prefix_idx == std::string::npos) {
      return "";
    }

    return source.substr(prefix_idx + 1);
  }

 public:
  Result ParseAndAppend(const std::string& option, ProfileSaverOptions& existing) {
    // Special case which doesn't include a wildcard argument definition.
    // We pass-it through as-is.
    if (option == "-Xjitsaveprofilinginfo") {
      existing.enabled_ = true;
      return Result::SuccessNoValue();
    }

    if (option == "profile-boot-class-path") {
      existing.profile_boot_class_path_ = true;
      return Result::SuccessNoValue();
    }

    if (option == "profile-aot-code") {
      existing.profile_aot_code_ = true;
      return Result::SuccessNoValue();
    }

    if (option == "save-without-jit-notifications") {
      existing.wait_for_jit_notifications_to_save_ = false;
      return Result::SuccessNoValue();
    }

    // The rest of these options are always the wildcard from '-Xps-*'
    std::string suffix = RemovePrefix(option);

    if (option.starts_with("min-save-period-ms:")) {
      CmdlineType<unsigned int> type_parser;
      return ParseInto(existing,
             &ProfileSaverOptions::min_save_period_ms_,
             type_parser.Parse(suffix));
    }
    if (option.starts_with("min-first-save-ms:")) {
      CmdlineType<unsigned int> type_parser;
      return ParseInto(existing,
             &ProfileSaverOptions::min_first_save_ms_,
             type_parser.Parse(suffix));
    }
    if (option.starts_with("save-resolved-classes-delay-ms:")) {
      LOG(WARNING) << "-Xps-save-resolved-classes-delay-ms is deprecated";
      return Result::SuccessNoValue();
    }
    if (option.starts_with("hot-startup-method-samples:")) {
      LOG(WARNING) << "-Xps-hot-startup-method-samples option is deprecated";
      return Result::SuccessNoValue();
    }
    if (option.starts_with("min-methods-to-save:")) {
      CmdlineType<unsigned int> type_parser;
      return ParseInto(existing,
             &ProfileSaverOptions::min_methods_to_save_,
             type_parser.Parse(suffix));
    }
    if (option.starts_with("min-classes-to-save:")) {
      CmdlineType<unsigned int> type_parser;
      return ParseInto(existing,
             &ProfileSaverOptions::min_classes_to_save_,
             type_parser.Parse(suffix));
    }
    if (option.starts_with("min-notification-before-wake:")) {
      CmdlineType<unsigned int> type_parser;
      return ParseInto(existing,
             &ProfileSaverOptions::min_notification_before_wake_,
             type_parser.Parse(suffix));
    }
    if (option.starts_with("max-notification-before-wake:")) {
      CmdlineType<unsigned int> type_parser;
      return ParseInto(existing,
             &ProfileSaverOptions::max_notification_before_wake_,
             type_parser.Parse(suffix));
    }
    if (option.starts_with("inline-cache-threshold:")) {
      CmdlineType<uint16_t> type_parser;
      return ParseInto(
          existing, &ProfileSaverOptions::inline_cache_threshold_, type_parser.Parse(suffix));
    }
    if (option.starts_with("profile-path:")) {
      existing.profile_path_ = suffix;
      return Result::SuccessNoValue();
    }

    return Result::Failure(std::string("Invalid suboption '") + option + "'");
  }

  static const char* Name() { return "ProfileSaverOptions"; }
  static const char* DescribeType() { return "string|unsigned integer"; }
  static constexpr bool kCanParseBlankless = true;
};

template<>
struct CmdlineType<ExperimentalFlags> : CmdlineTypeParser<ExperimentalFlags> {
  Result ParseAndAppend(const std::string& option, ExperimentalFlags& existing) {
    if (option == "none") {
      existing = ExperimentalFlags::kNone;
    } else {
      return Result::Failure(std::string("Unknown option '") + option + "'");
    }
    return Result::SuccessNoValue();
  }

  static const char* Name() { return "ExperimentalFlags"; }
  static const char* DescribeType() { return "none"; }
};
}  // namespace art
#endif  // ART_CMDLINE_CMDLINE_TYPES_H_
