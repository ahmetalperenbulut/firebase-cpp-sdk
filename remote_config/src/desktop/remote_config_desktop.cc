// Copyright 2017 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "remote_config/src/desktop/remote_config_desktop.h"

#include <cctype>
#include <chrono>  // NOLINT
#include <cstdint>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <thread>  // NOLINT
#include <vector>

#include "firebase/app.h"
#include "firebase/future.h"
#include "remote_config/src/common.h"
#include "remote_config/src/desktop/config_data.h"
#include "remote_config/src/desktop/file_manager.h"
#include "remote_config/src/desktop/rest.h"
#include "remote_config/src/include/firebase/remote_config.h"

#ifndef SWIG
#include "firebase/variant.h"
#endif  // SWIG

namespace firebase {
namespace remote_config {
namespace internal {

const char* const RemoteConfigDesktop::kDefaultNamespace = "configns:firebase";
const char* const RemoteConfigDesktop::kDefaultValueForString = "";
const int64_t RemoteConfigDesktop::kDefaultValueForLong = 0L;
const double RemoteConfigDesktop::kDefaultValueForDouble = 0.0;
const bool RemoteConfigDesktop::kDefaultValueForBool = false;

RemoteConfigDesktop::RemoteConfigDesktop(
    const firebase::App& app, const RemoteConfigFileManager& file_manager)
    : app_(app),
      file_manager_(file_manager),
      is_fetch_process_have_task_(false) {
  file_manager.Load(&configs_);
  AsyncSaveToFile();
  AsyncFetch();
}

RemoteConfigDesktop::~RemoteConfigDesktop() {
  fetch_channel_.Close();
  if (fetch_thread_.joinable()) {
    fetch_thread_.join();
  }

  save_channel_.Close();
  if (save_thread_.joinable()) {
    save_thread_.join();
  }
}

void RemoteConfigDesktop::AsyncSaveToFile() {
  save_thread_ = std::thread([this]() {
    while (save_channel_.Get()) {
      LayeredConfigs copy;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        copy = configs_;
      }
      file_manager_.Save(copy);
    }
  });
}

std::string RemoteConfigDesktop::VariantToString(const Variant& variant,
                                                 bool* failure) {
  if (variant.is_blob()) {
    const uint8_t* blob_data = variant.blob_data();
    size_t blob_size = variant.blob_size();
    std::string s;
    s.resize(blob_size);
    for (size_t i = 0; i < blob_size; i++) {
      s[i] = static_cast<char>(blob_data[i]);
    }
    return s;
  }

  if (!variant.is_bool() && !variant.is_int64() && !variant.is_double() &&
      !variant.is_string()) {
    *failure = true;
    return "";
  }
  return variant.AsString().string_value();
}

#ifndef SWIG
void RemoteConfigDesktop::SetDefaults(const ConfigKeyValueVariant* defaults,
                                      size_t number_of_defaults) {
  if (defaults == nullptr) {
    return;
  }
  std::map<std::string, std::string> defaults_map;
  for (size_t i = 0; i < number_of_defaults; i++) {
    const char* key = defaults[i].key;
    bool failure = false;
    std::string value = VariantToString(defaults[i].value, &failure);
    if (key && !failure) {
      defaults_map[key] = value;
    }
  }
  SetDefaults(defaults_map);
}
#endif  // SWIG

void RemoteConfigDesktop::SetDefaults(const ConfigKeyValue* defaults,
                                      size_t number_of_defaults) {
  if (defaults == nullptr) {
    return;
  }
  std::map<std::string, std::string> defaults_map;
  for (size_t i = 0; i < number_of_defaults; i++) {
    const char* key = defaults[i].key;
    const char* value = defaults[i].value;
    if (key && value) {
      defaults_map[key] = value;
    }
  }
  SetDefaults(defaults_map);
}

void RemoteConfigDesktop::SetDefaults(
    const std::map<std::string, std::string>& defaults_map) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    configs_.defaults.SetNamespace(defaults_map, kDefaultNamespace);
  }
  save_channel_.Put();
}

std::string RemoteConfigDesktop::GetConfigSetting(ConfigSetting setting) {
  std::unique_lock<std::mutex> lock(mutex_);
  return configs_.metadata.GetSetting(setting);
}

void RemoteConfigDesktop::SetConfigSetting(ConfigSetting setting,
                                           const char* value) {
  if (value == nullptr) {
    return;
  }
  {
    std::unique_lock<std::mutex> lock(mutex_);
    configs_.metadata.AddSetting(setting, value);
  }
  save_channel_.Put();
}

bool RemoteConfigDesktop::CheckValueInActiveAndDefault(const char* key,
                                                       ValueInfo* info,
                                                       std::string* value) {
  return CheckValueInConfig(configs_.active, kValueSourceRemoteValue, key, info,
                            value) ||
         CheckValueInConfig(configs_.defaults, kValueSourceDefaultValue, key,
                            info, value);
}

bool RemoteConfigDesktop::CheckValueInConfig(const NamespacedConfigData& config,
                                             ValueSource source,
                                             const char* key, ValueInfo* info,
                                             std::string* value) {
  if (!key) return false;

  {
    // TODO(b/74461360): Replace the thread locks with firebase ones.
    std::unique_lock<std::mutex> lock(mutex_);
    if (!config.HasValue(key, kDefaultNamespace)) {
      return false;
    }
    *value = config.GetValue(key, kDefaultNamespace);
  }

  if (info) info->source = source;
  return true;
}

bool RemoteConfigDesktop::IsBoolTrue(const std::string& str) {
  // regex: ^(1|true|t|yes|y|on)$
  return str == "1" || str == "true" || str == "t" || str == "yes" ||
         str == "y" || str == "on";
}

bool RemoteConfigDesktop::IsBoolFalse(const std::string& str) {
  // regex: ^(0|false|f|no|n|off)$
  return str == "0" || str == "false" || str == "f" || str == "no" ||
         str == "n" || str == "off";
}

bool RemoteConfigDesktop::IsLong(const std::string& str) {
  // regex: ^[-+]?[0-9]+$
  // Don't allow empty string.
  if (str.length() == 0) return false;
  // Don't allow leading space (strtol trims it).
  if (isspace(str[0])) return false;
  char* endptr;
  (void)strtol(str.c_str(), &endptr, 10);  // NOLINT
  return (*endptr == '\0');  // Ensure we consumed the whole string.
}

bool RemoteConfigDesktop::IsDouble(const std::string& str) {
  // regex: ^[-+]?[0-9]*.?[0-9]+([eE][-+]?[0-9]+)?
  // Don't allow empty string.
  if (str.length() == 0) return false;
  // Don't allow leading space (strtod trims it).
  if (isspace(str[0])) return false;
  char* endptr;
  (void)strtod(str.c_str(), &endptr);
  return (*endptr == '\0');  // Ensure we consumed the whole string.
}

bool RemoteConfigDesktop::GetBoolean(const char* key,
                                     ValueInfo* info) {
  std::string value;
  if (!CheckValueInActiveAndDefault(key, info, &value)) {
    if (info) {
      info->source = kValueSourceStaticValue;
      info->conversion_successful = true;
    }
    return kDefaultValueForBool;
  }

  if (IsBoolTrue(value)) {
    if (info) info->conversion_successful = true;
    return true;
  }
  if (IsBoolFalse(value)) {
    if (info) info->conversion_successful = true;
    return false;
  }
  if (info) info->conversion_successful = false;
  return kDefaultValueForBool;
}

std::string RemoteConfigDesktop::GetString(const char* key,
                                           ValueInfo* info) {
  std::string value;
  if (!CheckValueInActiveAndDefault(key, info, &value)) {
    if (info) {
      info->source = kValueSourceStaticValue;
      info->conversion_successful = true;
    }
    return kDefaultValueForString;
  }

  if (info) info->conversion_successful = true;
  return value;
}

int64_t RemoteConfigDesktop::GetLong(const char* key,
                                     ValueInfo* info) {
  std::string value;
  if (!CheckValueInActiveAndDefault(key, info, &value)) {
    if (info) {
      info->source = kValueSourceStaticValue;
      info->conversion_successful = true;
    }
    return kDefaultValueForLong;
  }

  int64_t long_value = kDefaultValueForLong;
  bool convertation_failure = !IsLong(value);

  std::stringstream convertor;
  convertor << value;
  convertor >> long_value;
  convertation_failure = convertation_failure || convertor.fail();

  if (info) info->conversion_successful = !convertation_failure;
  return convertation_failure ? kDefaultValueForLong : long_value;
}

double RemoteConfigDesktop::GetDouble(const char* key,
                                      ValueInfo* info) {
  std::string value;
  if (!CheckValueInActiveAndDefault(key, info, &value)) {
    if (info) {
      info->source = kValueSourceStaticValue;
      info->conversion_successful = true;
    }
    return kDefaultValueForDouble;
  }

  double double_value = kDefaultValueForDouble;
  bool convertation_failure = !IsDouble(value);

  std::stringstream convertor;
  convertor << value;
  convertor >> double_value;
  convertation_failure = convertation_failure || convertor.fail();

  if (info) info->conversion_successful = !convertation_failure;
  return convertation_failure ? kDefaultValueForDouble : double_value;
}

std::vector<unsigned char> RemoteConfigDesktop::GetData(const char* key,
                                                        ValueInfo* info) {
  std::string value;
  if (!CheckValueInActiveAndDefault(key, info, &value)) {
    if (info) {
      info->source = kValueSourceStaticValue;
      info->conversion_successful = true;
    }
    return kDefaultValueForData;
  }

  std::vector<unsigned char> data_value;
  data_value.reserve(value.size());
  for (const char& ch : value)
    data_value.push_back(static_cast<unsigned char>(ch));

  if (info) info->conversion_successful = true;
  return data_value;
}

std::vector<std::string> RemoteConfigDesktop::GetKeys() {
  return GetKeysByPrefix("");
}

std::vector<std::string> RemoteConfigDesktop::GetKeysByPrefix(
    const char* prefix) {
  if (prefix == nullptr) return std::vector<std::string>();
  std::set<std::string> unique_keys;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    configs_.active.GetKeysByPrefix(prefix, kDefaultNamespace, &unique_keys);
    configs_.defaults.GetKeysByPrefix(prefix, kDefaultNamespace, &unique_keys);
  }
  return std::vector<std::string>(unique_keys.begin(), unique_keys.end());
}

bool RemoteConfigDesktop::ActivateFetched() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    // Fetched config not found or already activated.
    if (configs_.fetched.timestamp() <= configs_.active.timestamp())
      return false;
    configs_.active = configs_.fetched;
  }
  save_channel_.Put();
  return true;
}

const ConfigInfo& RemoteConfigDesktop::GetInfo() const {
  std::unique_lock<std::mutex> lock(mutex_);
  return configs_.metadata.info();
}

void RemoteConfigDesktop::AsyncFetch() {
  fetch_thread_ = std::thread([this]() {
    SafeFutureHandle<void> handle;
    while (fetch_channel_.Get()) {
      RemoteConfigREST* rest = nullptr;

      {
        std::unique_lock<std::mutex> lock(mutex_);
        handle = fetch_handle_;
        rest = new RemoteConfigREST(app_.options(), configs_,
                                    cache_expiration_in_seconds_);
      }

      // Fetch fresh config from server.
      rest->Fetch(app_);

      {
        std::unique_lock<std::mutex> lock(mutex_);

        // Need to copy everything to `configs_.fetched`.
        configs_.fetched = rest->fetched();

        // Need to copy only info and digests to `configs_.metadata`.
        const RemoteConfigMetadata& metadata = rest->metadata();
        configs_.metadata.set_info(metadata.info());
        configs_.metadata.set_digest_by_namespace(
            metadata.digest_by_namespace());

        delete rest;

        is_fetch_process_have_task_ = false;
      }
      FetchFutureStatus futureResult =
          (GetInfo().last_fetch_status == kLastFetchStatusSuccess)
          ? kFetchFutureStatusSuccess : kFetchFutureStatusFailure;

      FutureData* future_data = FutureData::Get();
      future_data->api()->Complete(handle, futureResult);
    }
  });
}

Future<void> RemoteConfigDesktop::Fetch(uint64_t cache_expiration_in_seconds) {
  std::unique_lock<std::mutex> lock(mutex_);

  uint64_t milliseconds_since_epoch =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  uint64_t cache_expiration_timestamp =
      configs_.fetched.timestamp() + 1000 * cache_expiration_in_seconds;

  // Need to fetch in two cases:
  // - we are not fetching at this moment
  // - cache_expiration_in_seconds is zero
  // or
  // - we are not fetching at this moment
  // - cache (fetched) data is older than cache_expiration_in_seconds
  if (!is_fetch_process_have_task_ &&
      ((cache_expiration_in_seconds == 0) ||
       (cache_expiration_timestamp < milliseconds_since_epoch))) {
    ReferenceCountedFutureImpl* api = FutureData::Get()->api();
    fetch_handle_ = api->SafeAlloc<void>(kRemoteConfigFnFetch);
    is_fetch_process_have_task_ = true;
    fetch_channel_.Put();
    cache_expiration_in_seconds_ = cache_expiration_in_seconds;
  }
  return FetchLastResult();
}

Future<void> RemoteConfigDesktop::FetchLastResult() {
  ReferenceCountedFutureImpl* api = FutureData::Get()->api();
  return static_cast<const Future<void>&>(
      api->LastResult(kRemoteConfigFnFetch));
}

}  // namespace internal
}  // namespace remote_config
}  // namespace firebase
