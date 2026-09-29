/*
 * Copyright 2026 AERA Recovery Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <json/json.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#ifndef AERA_CLI_VERSION
#define AERA_CLI_VERSION "development"
#endif

namespace {

constexpr const char* kInputPath = "/system/bin/aerain";
constexpr const char* kOutputPath = "/system/bin/aeraout";
constexpr const char* kCancelPath = "/system/bin/aeracancel";
constexpr size_t kMaxEventBuffer = 96U * 1024U * 1024U;

struct GlobalOptions {
  bool json = false;
  bool quiet = false;
  bool dry_run = false;
  int timeout_seconds = 0;
};

struct BuiltRequest {
  Json::Value root{Json::objectValue};
  std::string error;
  bool ok() const { return error.empty(); }
};

volatile sig_atomic_t g_interrupts = 0;

void PrintUsage(FILE* out) {
  std::fprintf(out,
      "AERA Recovery command line\n\n"
      "Usage: aera [global options] <command> [arguments]\n\n"
      "Global options:\n"
      "  --json              Emit the original NDJSON event stream\n"
      "  --quiet             Hide log and progress events\n"
      "  --timeout SECONDS   Stop waiting after the given time\n"
      "  --dry-run           Print the request without contacting recovery\n"
      "  --version           Print the client version\n"
      "  --help              Show this help\n\n"
      "Recovery commands:\n"
      "  status | storages | log | sideload\n"
      "  mount [PATH...]             unmount [PATH...]\n"
      "  flash ZIP...\n"
      "  backup --parts LIST [--name NAME] [--compress]\n"
      "  restore --name NAME [--parts LIST] [--no-digest-check]\n"
      "  decrypt [PASSWORD|--password-stdin] [--user ID]\n"
      "  wipe [PARTITION...]         format-data --confirm\n"
      "  transition fastboot|recovery\n"
      "  reboot TARGET               wifi ACTION [options]\n"
      "  partition PATH ACTION [FS]  mtp ACTION\n\n"
      "Reserved (unsupported for now):\n"
      "  reflash, addons, control, input, screencap\n"
      "  adbd, ota, ors, ors-cmd\n"
      "  flash --verify, --reflash, --unmount-system, --unmount-vendor\n\n"
      "Protocol access:\n"
      "  call OP [--string K V|--int K V|--bool K V|--list K CSV]...\n"
      "  rpc JSON                    Send a complete AERA RPC request\n\n"
      "Press Ctrl+C once to request safe cancellation; twice to exit.\n");
}

std::string CompactJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["commentStyle"] = "None";
  return Json::writeString(builder, value);
}

std::string PrettyJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "  ";
  builder["commentStyle"] = "None";
  return Json::writeString(builder, value);
}

bool ParseJson(std::string_view text, Json::Value* value, std::string* error) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  return reader->parse(text.data(), text.data() + text.size(), value, error);
}

bool ParseInt(const std::string& text, int* value) {
  if (text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (errno || end == text.c_str() || *end || parsed < INT_MIN || parsed > INT_MAX)
    return false;
  *value = static_cast<int>(parsed);
  return true;
}

bool ParseBool(const std::string& text, bool* value) {
  if (text == "true" || text == "1" || text == "yes" || text == "on") {
    *value = true;
    return true;
  }
  if (text == "false" || text == "0" || text == "no" || text == "off") {
    *value = false;
    return true;
  }
  return false;
}

std::vector<std::string> SplitCsv(const std::string& text) {
  std::vector<std::string> values;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = text.find(',', start);
    const size_t end = comma == std::string::npos ? text.size() : comma;
    if (end > start) values.emplace_back(text.substr(start, end - start));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return values;
}

Json::Value StringArray(const std::vector<std::string>& values) {
  Json::Value array(Json::arrayValue);
  for (const auto& value : values) array.append(value);
  return array;
}

bool ValidKey(const std::string& key) {
  if (key.empty()) return false;
  for (char c : key) {
    if (!(c == '_' || c == '-' || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      return false;
  }
  return true;
}

std::string ReadSecretLine() {
  std::string result;
  char buffer[512];
  if (!std::fgets(buffer, sizeof(buffer), stdin)) return result;
  result = buffer;
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();
  return result;
}

std::string RequestId() {
  char id[64];
  std::snprintf(id, sizeof(id), "aera-%ld-%ld", static_cast<long>(getpid()),
                static_cast<long>(time(nullptr)));
  return id;
}

BuiltRequest Error(const std::string& message) {
  BuiltRequest result;
  result.error = message;
  return result;
}

BuiltRequest BaseRequest(const std::string& op) {
  BuiltRequest result;
  result.root["v"] = 1;
  result.root["id"] = RequestId();
  result.root["op"] = op;
  result.root["args"] = Json::Value(Json::objectValue);
  return result;
}

bool NeedValue(const std::vector<std::string>& args, size_t index,
               const std::string& option, BuiltRequest* out) {
  if (index + 1 < args.size()) return true;
  out->error = option + " requires a value";
  return false;
}

BuiltRequest BuildCall(const std::vector<std::string>& args) {
  if (args.size() < 2) return Error("call requires an operation name");
  BuiltRequest out = BaseRequest(args[1]);
  Json::Value& values = out.root["args"];
  for (size_t i = 2; i < args.size();) {
    const std::string type = args[i];
    if (type != "--string" && type != "--int" && type != "--bool" &&
        type != "--list" && type != "--null")
      return Error("unknown call field option: " + type);
    if (!NeedValue(args, i, type, &out)) return out;
    const std::string key = args[++i];
    if (!ValidKey(key)) return Error("invalid argument key: " + key);
    if (type == "--null") {
      values[key] = Json::Value();
      ++i;
      continue;
    }
    if (!NeedValue(args, i, type + " " + key, &out)) return out;
    const std::string value = args[++i];
    if (type == "--string") values[key] = value;
    if (type == "--list") values[key] = StringArray(SplitCsv(value));
    if (type == "--int") {
      int parsed = 0;
      if (!ParseInt(value, &parsed)) return Error("invalid integer for " + key);
      values[key] = parsed;
    }
    if (type == "--bool") {
      bool parsed = false;
      if (!ParseBool(value, &parsed)) return Error("invalid boolean for " + key);
      values[key] = parsed;
    }
    ++i;
  }
  return out;
}

BuiltRequest BuildRawRpc(const std::vector<std::string>& args) {
  if (args.size() != 2) return Error("rpc requires one JSON request argument");
  BuiltRequest out;
  std::string parse_error;
  if (!ParseJson(args[1], &out.root, &parse_error) || !out.root.isObject())
    return Error("invalid RPC JSON: " + parse_error);
  if (!out.root.isMember("v")) out.root["v"] = 1;
  if (!out.root.isMember("id")) out.root["id"] = RequestId();
  if (!out.root.isMember("args")) out.root["args"] = Json::Value(Json::objectValue);
  if (!out.root["op"].isString() || out.root["op"].asString().empty())
    return Error("RPC request requires a non-empty op");
  return out;
}

BuiltRequest BuildFlash(const std::vector<std::string>& args) {
  BuiltRequest out = BaseRequest("flash");
  std::vector<std::string> zips;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--verify" || args[i] == "--reflash" ||
        args[i] == "--unmount-system" || args[i] == "--unmount-vendor")
      return Error(args[i] + " is reserved but unsupported for now");
    else if (args[i].rfind("--", 0) == 0) return Error("unknown flash option: " + args[i]);
    else zips.push_back(args[i]);
  }
  if (zips.empty()) return Error("flash requires at least one ZIP path");
  out.root["args"]["zips"] = StringArray(zips);
  return out;
}

BuiltRequest BuildBackupRestore(const std::vector<std::string>& args, bool restore) {
  BuiltRequest out = BaseRequest(restore ? "restore" : "backup");
  Json::Value& values = out.root["args"];
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--compress" && !restore) values["compress"] = true;
    else if (arg == "--no-digest" && !restore) values["digest"] = false;
    else if (arg == "--no-digest-check" && restore) values["digest_check"] = false;
    else if (arg == "--parts" || arg == "--name" || arg == "--path" ||
             (!restore && arg == "--storage")) {
      if (!NeedValue(args, i, arg, &out)) return out;
      const std::string value = args[++i];
      if (arg == "--parts") values["parts"] = StringArray(SplitCsv(value));
      else values[arg.substr(2)] = value;
    } else {
      return Error("unknown " + std::string(restore ? "restore" : "backup") +
                   " option: " + arg);
    }
  }
  if (!restore && (!values.isMember("parts") || values["parts"].empty()))
    return Error("backup requires --parts with a comma-separated partition list");
  if (restore && !values.isMember("name") && !values.isMember("path"))
    return Error("restore requires --name or --path");
  return out;
}

BuiltRequest BuildWifi(const std::vector<std::string>& args) {
  BuiltRequest out = BaseRequest("wlan");
  Json::Value& values = out.root["args"];
  values["action"] = args.size() > 1 ? args[1] : "status";
  for (size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--password-stdin") values["password"] = ReadSecretLine();
    else if (args[i] == "--ssid" || args[i] == "--password") {
      if (!NeedValue(args, i, args[i], &out)) return out;
      const std::string key = args[i].substr(2);
      values[key] = args[++i];
    } else if (!values.isMember("ssid")) values["ssid"] = args[i];
    else return Error("unexpected wifi argument: " + args[i]);
  }
  return out;
}

BuiltRequest BuildInput(const std::vector<std::string>& args) {
  if (args.size() < 2) return Error("input requires an action");
  BuiltRequest out = BaseRequest("input");
  Json::Value& values = out.root["args"];
  values["action"] = args[1];
  auto number = [&](size_t index, const char* key) -> bool {
    if (index >= args.size()) {
      out.error = std::string("input ") + args[1] + " requires more coordinates";
      return false;
    }
    int parsed = 0;
    if (!ParseInt(args[index], &parsed)) {
      out.error = "invalid input coordinate: " + args[index];
      return false;
    }
    values[key] = parsed;
    return true;
  };
  if (args[1] == "key") {
    if (args.size() != 3) return Error("input key requires a key name");
    values["key"] = args[2];
  } else if (args[1] == "tap" || args[1] == "down" || args[1] == "move" || args[1] == "up") {
    if (args.size() != 4 || !number(2, "x") || !number(3, "y")) return out.ok() ? Error("input action requires X Y") : out;
  } else if (args[1] == "swipe") {
    if (args.size() != 7 || !number(2, "x") || !number(3, "y") ||
        !number(4, "x2") || !number(5, "y2") || !number(6, "duration"))
      return out.ok() ? Error("input swipe requires X Y X2 Y2 DURATION_MS") : out;
  } else {
    return Error("unsupported input action: " + args[1]);
  }
  return out;
}

BuiltRequest BuildGroup(const std::vector<std::string>& args) {
  BuiltRequest out = BaseRequest(args[0]);
  Json::Value& values = out.root["args"];
  std::vector<std::string> positional;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--no-auth" || args[i] == "--generate") {
      values[args[i].substr(2)] = true;
    } else if (args[i] == "--port" || args[i] == "--timeout" || args[i] == "--fps") {
      if (!NeedValue(args, i, args[i], &out)) return out;
      int parsed = 0;
      if (!ParseInt(args[++i], &parsed)) return Error("invalid value for " + args[i - 1]);
      values[args[i - 1].substr(2)] = parsed;
    } else if (args[i] == "--aera-id" || args[i] == "--type") {
      if (!NeedValue(args, i, args[i], &out)) return out;
      const std::string key = args[i] == "--aera-id" ? "aera_id" : "type";
      values[key] = args[++i];
    } else {
      positional.push_back(args[i]);
    }
  }
  if (!positional.empty()) values["action"] = positional[0];
  if (positional.size() > 1) values["subaction"] = positional[1];
  if (positional.size() > 2)
    values["values"] = StringArray({positional.begin() + 2, positional.end()});
  return out;
}

BuiltRequest BuildRequest(const std::vector<std::string>& args) {
  if (args.empty()) return Error("missing command");
  const std::string& command = args[0];
  if (command == "reflash" || command == "addons" || command == "control" ||
      command == "input" || command == "screencap" || command == "adbd" ||
      command == "ota" || command == "ors" || command == "ors-cmd")
    return Error(command + " is reserved but unsupported for now");
  if (command == "rpc") return BuildRawRpc(args);
  if (command == "call") return BuildCall(args);
  if (command == "flash" || command == "install") return BuildFlash(args);
  if (command == "backup") return BuildBackupRestore(args, false);
  if (command == "restore") return BuildBackupRestore(args, true);
  if (command == "wifi" || command == "wlan") return BuildWifi(args);
  if (command == "input") return BuildInput(args);
  if (command == "adbd" || command == "ota") return BuildGroup(args);

  if (command == "status" || command == "storages" || command == "log" ||
      command == "sideload" || command == "reflash") {
    if (args.size() != 1) return Error(command + " takes no arguments");
    return BaseRequest(command);
  }
  if (command == "mount" || command == "unmount" || command == "umount" || command == "wipe") {
    BuiltRequest out = BaseRequest(command == "umount" ? "unmount" : command);
    const std::vector<std::string> items(args.begin() + 1, args.end());
    if (!items.empty()) out.root["args"][command == "wipe" ? "parts" : "paths"] = StringArray(items);
    return out;
  }
  if (command == "format-data") {
    if (args.size() != 2 || args[1] != "--confirm")
      return Error("format-data requires --confirm");
    BuiltRequest out = BaseRequest("format_data");
    out.root["args"]["confirm"] = true;
    return out;
  }
  if (command == "reboot") {
    if (args.size() != 2) return Error("reboot requires a target");
    if (args[1] == "fastboot")
      return Error("use 'aera transition fastboot'; hardware bootloader is 'aera reboot bootloader'");
    BuiltRequest out = BaseRequest("reboot");
    out.root["args"]["target"] = args[1];
    return out;
  }
  if (command == "transition") {
    if (args.size() != 2 ||
        (args[1] != "fastboot" && args[1] != "recovery"))
      return Error("transition requires fastboot or recovery");
    BuiltRequest out = BaseRequest("transition");
    out.root["args"]["target"] = args[1];
    return out;
  }
  if (command == "decrypt") {
    BuiltRequest out = BaseRequest("decrypt");
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--password-stdin") out.root["args"]["password"] = ReadSecretLine();
      else if (args[i] == "--user") {
        if (!NeedValue(args, i, args[i], &out)) return out;
        int user = 0;
        if (!ParseInt(args[++i], &user)) return Error("invalid decryption user");
        out.root["args"]["user"] = user;
      } else if (!out.root["args"].isMember("password")) out.root["args"]["password"] = args[i];
      else return Error("unexpected decrypt argument: " + args[i]);
    }
    if (!out.root["args"].isMember("password")) return Error("decrypt requires a password or --password-stdin");
    return out;
  }
  if (command == "partition") {
    if (args.size() < 2 || args.size() > 4) return Error("partition requires PATH [ACTION] [FS]");
    BuiltRequest out = BaseRequest("partition");
    out.root["args"]["path"] = args[1];
    if (args.size() > 2) out.root["args"]["action"] = args[2];
    if (args.size() > 3) out.root["args"]["fs"] = args[3];
    return out;
  }
  if (command == "addons") {
    BuiltRequest out = BaseRequest("addons");
    if (args.size() > 1) out.root["args"]["action"] = args[1];
    if (args.size() > 2)
      out.root["args"]["names"] = StringArray({args.begin() + 2, args.end()});
    return out;
  }
  if (command == "mtp" || command == "control") {
    BuiltRequest out = BaseRequest(command);
    out.root["args"]["action"] = args.size() > 1 ? args[1] : "status";
    if (args.size() > 2) return Error(command + " accepts one action");
    return out;
  }
  if (command == "screencap") {
    BuiltRequest out = BaseRequest("screencap");
    if (args.size() == 2 && args[1] == "--base64") out.root["args"]["base64"] = true;
    else if (args.size() == 2) out.root["args"]["path"] = args[1];
    else if (args.size() > 2) return Error("screencap accepts a path or --base64");
    return out;
  }
  if (command == "ors") {
    if (args.size() != 2) return Error("ors requires a script path");
    BuiltRequest out = BaseRequest("ors");
    out.root["args"]["path"] = args[1];
    return out;
  }
  if (command == "ors-cmd") {
    if (args.size() < 2) return Error("ors-cmd requires command tokens");
    BuiltRequest out = BaseRequest("ors-cmd");
    out.root["args"]["raw"] = StringArray({args.begin() + 1, args.end()});
    return out;
  }
  return Error("unknown command: " + command);
}

void CancelHandler(int) {
  ++g_interrupts;
  const int fd = open(kCancelPath, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd >= 0) {
    const char value = '1';
    (void)write(fd, &value, 1);
    close(fd);
  }
  if (g_interrupts > 1) _exit(130);
}

bool WriteAll(int fd, const std::string& data) {
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t count = write(fd, data.data() + written, data.size() - written);
    if (count > 0) written += static_cast<size_t>(count);
    else if (count < 0 && errno == EINTR) continue;
    else return false;
  }
  return true;
}

void PrintScalar(const Json::Value& value) {
  if (value.isString()) std::printf("%s", value.asCString());
  else if (value.isBool()) std::printf("%s", value.asBool() ? "yes" : "no");
  else if (value.isInt64()) std::printf("%lld", static_cast<long long>(value.asInt64()));
  else if (value.isUInt64()) std::printf("%llu", static_cast<unsigned long long>(value.asUInt64()));
  else if (value.isDouble()) std::printf("%g", value.asDouble());
  else if (value.isNull()) std::printf("-");
  else std::printf("%s", CompactJson(value).c_str());
}

void PrintData(const Json::Value& value) {
  if (value.isObject()) {
    for (const auto& key : value.getMemberNames()) {
      std::printf("%-24s ", key.c_str());
      PrintScalar(value[key]);
      std::printf("\n");
    }
    return;
  }
  if (value.isArray()) {
    for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
      if (value[i].isObject()) {
        std::printf("[%u]", i + 1);
        for (const auto& key : value[i].getMemberNames()) {
          std::printf("  %s=", key.c_str());
          PrintScalar(value[i][key]);
        }
        std::printf("\n");
      } else {
        PrintScalar(value[i]);
        std::printf("\n");
      }
    }
    return;
  }
  PrintScalar(value);
  std::printf("\n");
}

int HandleEvent(const std::string& line, const GlobalOptions& options,
                bool* finished, bool* progress_active) {
  if (options.json) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  }
  Json::Value event;
  std::string error;
  if (!ParseJson(line, &event, &error) || !event.isObject()) {
    if (!options.json) std::fprintf(stderr, "AERA RPC returned malformed data: %s\n", error.c_str());
    return 2;
  }
  const std::string type = event["event"].asString();
  if (options.json) {
    if (type == "result") {
      *finished = true;
      return event["code"].asInt();
    }
    return -1;
  }
  if (type == "log") {
    if (!options.quiet) {
      if (*progress_active) std::printf("\n");
      *progress_active = false;
      std::fputs(event["text"].asCString(), stdout);
      std::fflush(stdout);
    }
  } else if (type == "progress") {
    if (!options.quiet) {
      const int percent = event.get("percent", 0).asInt();
      const std::string label = event.get("label", event.get("phase", "Working")).asString();
      if (isatty(STDOUT_FILENO)) {
        std::printf("\r\033[2K%-52s %3d%%", label.c_str(), percent);
        *progress_active = true;
      } else {
        std::printf("%s: %d%%\n", label.c_str(), percent);
      }
      std::fflush(stdout);
    }
  } else if (type == "data") {
    if (*progress_active) std::printf("\n");
    *progress_active = false;
    if (!options.quiet) {
      const std::string name = event.get("name", "data").asString();
      if (!name.empty()) std::printf("%s\n", name.c_str());
      const Json::Value& value = event["value"];
      if (value.isObject() && value.isMember("items")) PrintData(value["items"]);
      else PrintData(value);
    }
  } else if (type == "error") {
    if (*progress_active) std::printf("\n");
    *progress_active = false;
    std::fprintf(stderr, "AERA error [%s]: %s\n", event.get("code", "error").asCString(),
                 event.get("message", "operation failed").asCString());
  } else if (type == "result") {
    if (*progress_active) std::printf("\n");
    *progress_active = false;
    *finished = true;
    return event.get("code", 1).asInt();
  }
  return -1;
}

int SendRequest(const Json::Value& request, const GlobalOptions& options) {
  const int output = open(kOutputPath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (output < 0) {
    std::fprintf(stderr, "Cannot open AERA response channel %s: %s\n", kOutputPath, std::strerror(errno));
    return 3;
  }
  int input = open(kInputPath, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (input < 0) {
    std::fprintf(stderr, "Cannot open AERA command channel %s: %s\n", kInputPath, std::strerror(errno));
    close(output);
    return 3;
  }
  std::string payload = CompactJson(request);
  payload.push_back('\n');
  if (!WriteAll(input, payload)) {
    std::fprintf(stderr, "Could not send AERA request: %s\n", std::strerror(errno));
    close(input);
    close(output);
    return 3;
  }
  close(input);

  struct sigaction action {};
  action.sa_handler = CancelHandler;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  const int timeout_ms = options.timeout_seconds > 0 ? options.timeout_seconds * 1000 : -1;
  int elapsed_ms = 0;
  std::string buffer;
  bool finished = false;
  bool progress_active = false;
  int result = 2;
  while (!finished) {
    pollfd descriptor{output, POLLIN, 0};
    const int slice = timeout_ms < 0 ? 1000 : std::min(1000, timeout_ms - elapsed_ms);
    if (slice < 0 || (timeout_ms >= 0 && elapsed_ms >= timeout_ms)) {
      std::fprintf(stderr, "AERA command timed out\n");
      result = 124;
      break;
    }
    const int ready = poll(&descriptor, 1, slice);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) {
      std::fprintf(stderr, "AERA response wait failed: %s\n", std::strerror(errno));
      result = 3;
      break;
    }
    if (ready == 0) {
      if (timeout_ms >= 0) elapsed_ms += slice;
      continue;
    }
    char chunk[8192];
    const ssize_t count = read(output, chunk, sizeof(chunk));
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
    if (count <= 0) continue;
    buffer.append(chunk, static_cast<size_t>(count));
    if (buffer.size() > kMaxEventBuffer) {
      std::fprintf(stderr, "AERA response exceeded the safety limit\n");
      result = 3;
      break;
    }
    size_t newline = 0;
    while ((newline = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1);
      if (line.empty()) continue;
      const int event_result = HandleEvent(line, options, &finished, &progress_active);
      if (event_result >= 0) result = event_result;
    }
  }
  close(output);
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  GlobalOptions options;
  std::vector<std::string> command;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (command.empty() && arg == "--json") options.json = true;
    else if (command.empty() && arg == "--quiet") options.quiet = true;
    else if (command.empty() && arg == "--dry-run") options.dry_run = true;
    else if (command.empty() && arg == "--timeout") {
      if (++i >= argc || !ParseInt(argv[i], &options.timeout_seconds) || options.timeout_seconds <= 0) {
        std::fprintf(stderr, "--timeout requires a positive number of seconds\n");
        return 2;
      }
    } else if (command.empty() && (arg == "--help" || arg == "-h")) {
      PrintUsage(stdout);
      return 0;
    } else if (command.empty() && arg == "--version") {
      std::printf("AERA CLI %s\n", AERA_CLI_VERSION);
      return 0;
    } else {
      command.push_back(arg);
    }
  }
  if (command.empty()) {
    PrintUsage(stderr);
    return 2;
  }
  BuiltRequest request = BuildRequest(command);
  if (!request.ok()) {
    std::fprintf(stderr, "aera: %s\n\n", request.error.c_str());
    std::fprintf(stderr, "Run 'aera --help' for usage.\n");
    return 2;
  }
  if (options.dry_run) {
    std::printf("%s\n", options.json ? CompactJson(request.root).c_str() : PrettyJson(request.root).c_str());
    return 0;
  }
  return SendRequest(request.root, options);
}
