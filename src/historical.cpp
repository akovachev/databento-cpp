#include "databento/historical.hpp"

#include <httplib.h>

#include <atomic>
#include <cstdlib>  // get_env
#include <nlohmann/json.hpp>
#include <numeric>  // accumulate
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>  // move

#include "databento/constants.hpp"
#include "databento/datetime.hpp"
#include "databento/dbz_parser.hpp"
#include "databento/enums.hpp"
#include "databento/timeseries.hpp"

using databento::Historical;
using databento::HistoricalBuilder;

namespace {
std::string BuildBatchPath(const char* slug) {
  return std::string{"/v"} + databento::kApiVersionStr + "/batch" + slug;
}

std::string BuildMetadataPath(const char* slug) {
  return std::string{"/v"} + databento::kApiVersionStr + "/metadata" + slug;
}

std::string BuildSymbologyPath(const char* slug) {
  return std::string{"/v"} + databento::kApiVersionStr + "/symbology" + slug;
}

// std::string BuildTimeseriesPath(const char* slug) {
//   return std::string{"/v"} + databento::kApiVersionStr + "/timeseries" +
//   slug;
// }

void SetIfNotEmpty(httplib::Params* params, const std::string& key,
                   const std::string& value) {
  if (!value.empty()) {
    params->emplace(key, value);
  }
}

void SetIfNotEmpty(httplib::Params* params, const std::string& key,
                   const std::vector<std::string>& strings) {
  if (!strings.empty()) {
    std::string value =
        std::accumulate(strings.begin(), strings.end(), std::string{},
                        [](std::string acc, const std::string& str) {
                          return acc.empty() ? str : std::move(acc) + "," + str;
                        });
    params->emplace(key, std::move(value));
  }
}

void SetIfNotEmpty(httplib::Params* params, const std::string& key,
                   const std::vector<databento::BatchState>& states) {
  if (!states.empty()) {
    std::string value = std::accumulate(
        states.begin(), states.end(), std::string{},
        [](std::string acc, databento::BatchState state) {
          return acc.empty()
                     ? databento::ToString(state)
                     : std::move(acc) + "," + databento::ToString(state);
        });
    params->emplace(key, std::move(value));
  }
}

void SetIfPositive(httplib::Params* params, const std::string& key,
                   const std::size_t value) {
  if (value > 0) {
    params->emplace(key, std::to_string(value));
  }
}

std::runtime_error MissingKey(const std::string& endpoint,
                              const std::string& key) {
  std::ostringstream err_msg;
  err_msg << "Missing key '" << key << "' in response for " << endpoint;
  return std::runtime_error{err_msg.str()};
}

std::runtime_error TypeMismatch(const std::string& endpoint,
                                const std::string& expected_type,
                                const nlohmann::json& json) {
  std::ostringstream err_msg;
  err_msg << "Expected JSON " << expected_type << " response for " << endpoint
          << ", got " << json.type_name();
  return std::runtime_error{err_msg.str()};
}

std::runtime_error TypeMismatch(const std::string& endpoint,
                                const std::string& expected_type,
                                const nlohmann::json& key,
                                const nlohmann::json& value) {
  std::ostringstream err_msg;
  err_msg << "Expected " << expected_type << " values in JSON response for "
          << endpoint << ", got " << value.type_name() << " " << value
          << " for key " << key;
  throw std::runtime_error{err_msg.str()};
}

const nlohmann::json& CheckedAt(const std::string& endpoint,
                                const nlohmann::json& json,
                                const std::string& key) {
  if (json.contains(key)) {
    return json.at(key);
  }
  throw ::MissingKey(endpoint, key);
}

template <typename T>
T FromCheckedAtString(const std::string& endpoint, const nlohmann::json& json,
                      const std::string& key) {
  const auto& val_json = ::CheckedAt(endpoint, json, key);
  if (!val_json.is_string()) {
    throw ::TypeMismatch(endpoint, key + " string", val_json);
  }
  return databento::FromString<T>(val_json);
}

template <typename T>
T ParseAt(const std::string& endpoint, const nlohmann::json& json,
          const std::string& key);

template <>
bool ParseAt(const std::string& endpoint, const nlohmann::json& json,
             const std::string& key) {
  const auto& val_json = ::CheckedAt(endpoint, json, key);
  if (!val_json.is_boolean()) {
    throw ::TypeMismatch(endpoint, key + " bool", val_json);
  }
  return val_json;
}

template <>
std::string ParseAt(const std::string& endpoint, const nlohmann::json& json,
                    const std::string& key) {
  const auto& val_json = ::CheckedAt(endpoint, json, key);
  if (!val_json.is_string()) {
    throw ::TypeMismatch(endpoint, key + " string", val_json);
  }
  return val_json;
}

template <>
std::size_t ParseAt(const std::string& endpoint, const nlohmann::json& json,
                    const std::string& key) {
  const auto& val_json = ::CheckedAt(endpoint, json, key);
  if (!val_json.is_number_unsigned()) {
    throw ::TypeMismatch(endpoint, key + " unsigned number", val_json);
  }
  return val_json;
}

template <>
databento::EpochNanos ParseAt(const std::string& endpoint,
                              const nlohmann::json& json,
                              const std::string& key) {
  const auto& val_json = ::CheckedAt(endpoint, json, key);
  if (!val_json.is_number_unsigned()) {
    throw ::TypeMismatch(endpoint, key + " unsigned number", val_json);
  }
  return databento::EpochNanos{std::chrono::nanoseconds{val_json}};
}

template <>
std::vector<std::string> ParseAt(const std::string& endpoint,
                                 const nlohmann::json& json,
                                 const std::string& key) {
  const auto& symbols_json = ::CheckedAt(endpoint, json, key);
  if (!symbols_json.is_array()) {
    throw ::TypeMismatch(endpoint, key + " array", json);
  }
  std::vector<std::string> res;
  res.reserve(json.size());
  for (const auto& item : json.items()) {
    res.emplace_back(item.value());
  }
  return res;
}

databento::BatchJob Parse(const std::string& endpoint,
                          const nlohmann::json& json) {
  using databento::Compression;
  using databento::Delivery;
  using databento::DurationInterval;
  using databento::EpochNanos;
  using databento::Packaging;
  using databento::Schema;
  using databento::SType;

  if (!json.is_object()) {
    throw ::TypeMismatch(endpoint, "object", json);
  }
  databento::BatchJob res;
  res.id = ::CheckedAt(endpoint, json, "id");
  res.user_id = ParseAt<std::string>(endpoint, json, "user_id");
  res.bill_id = ParseAt<std::string>(endpoint, json, "bill_id");
  res.dataset = ParseAt<std::string>(endpoint, json, "dataset");
  res.symbols = ::ParseAt<std::vector<std::string>>(endpoint, json, "symbols");
  res.stype_in = ::FromCheckedAtString<SType>(endpoint, json, "stype_in");
  res.stype_out = ::FromCheckedAtString<SType>(endpoint, json, "stype_out");
  res.schema = ::FromCheckedAtString<Schema>(endpoint, json, "schema");
  res.start = ::ParseAt<EpochNanos>(endpoint, json, "start");
  res.end = ::ParseAt<EpochNanos>(endpoint, json, "end");
  res.limit = ::ParseAt<std::size_t>(endpoint, json, "limit");
  res.compression =
      ::FromCheckedAtString<Compression>(endpoint, json, "compression");
  res.split_duration =
      ::FromCheckedAtString<DurationInterval>(endpoint, json, "split_duration");
  res.split_size = ::ParseAt<std::size_t>(endpoint, json, "split_size");
  res.split_symbols = ::ParseAt<bool>(endpoint, json, "split_symbols");
  res.packaging = ::FromCheckedAtString<Packaging>(endpoint, json, "packaging");
  res.delivery = ::FromCheckedAtString<Delivery>(endpoint, json, "delivery");
  res.is_full_book = ::ParseAt<bool>(endpoint, json, "is_full_book");
  res.is_example = ::ParseAt<bool>(endpoint, json, "is_example");
  res.record_count = ::ParseAt<std::size_t>(endpoint, json, "record_count");
  res.billed_size = ::ParseAt<std::size_t>(endpoint, json, "billed_size");
  res.actual_size = ::ParseAt<std::size_t>(endpoint, json, "actual_size");
  res.package_size = ::ParseAt<std::size_t>(endpoint, json, "package_size");
  return res;
}
}  // namespace

Historical::Historical(std::string key, HistoricalGateway gateway)
    : key_{std::move(key)},
      gateway_{UrlFromGateway(gateway)},
      client_{key_, gateway_} {}

Historical::Historical(std::string key, std::string gateway, std::uint16_t port)
    : key_{std::move(key)},
      gateway_{std::move(gateway)},
      client_{key_, gateway_, port} {}

databento::BatchJob Historical::BatchSubmitJob(
    const std::string& dataset, Schema schema,
    const std::vector<std::string>& symbols, const std::string& start,
    const std::string& end) {
  return this->BatchSubmitJob(
      dataset, schema, symbols, start, end, DurationInterval::Day, {},
      Packaging::None, Delivery::Download, SType::Native, SType::ProductId, {});
}

databento::BatchJob Historical::BatchSubmitJob(
    const std::string& dataset, Schema schema,
    const std::vector<std::string>& symbols, const std::string& start,
    const std::string& end, DurationInterval split_duration,
    std::size_t split_size, Packaging packaging, Delivery delivery,
    SType stype_in, SType stype_out, std::size_t limit) {
  static const std::string kPath = ::BuildBatchPath(".submit_job");
  httplib::Params params{{"dataset", dataset},
                         {"schema", ToString(schema)},
                         {"encoding", "dbz"},
                         {"start", start},
                         {"end", end},
                         // {"compression", ToString(compression)},
                         {"split_duration", ToString(split_duration)},
                         {"packaging", ToString(packaging)},
                         {"delivery", ToString(delivery)},
                         {"stype_in", ToString(stype_in)},
                         {"stype_out", ToString(stype_out)}};
  ::SetIfPositive(&params, "split_size", split_size);
  ::SetIfPositive(&params, "limit", limit);
  ::SetIfNotEmpty(&params, "symbols", symbols);
  const nlohmann::json json = client_.PostJson(kPath, params);
  return ::Parse("BatchSubmitJob", json);
}

std::vector<databento::BatchJob> Historical::BatchListJobs() {
  static const std::vector<BatchState> kDefaultStates = {
      BatchState::Received, BatchState::Queued, BatchState::Processing,
      BatchState::Done};
  return this->BatchListJobs(kDefaultStates, {});
}

std::vector<databento::BatchJob> Historical::BatchListJobs(
    const std::vector<databento::BatchState>& states,
    const std::string& since) {
  static const std::string kEndpoint = "BatchListJobs";
  static const std::string kPath = ::BuildBatchPath(".list_jobs");

  httplib::Params params;
  ::SetIfNotEmpty(&params, "states", states);
  ::SetIfNotEmpty(&params, "since", since);
  const nlohmann::json json = client_.GetJson(kPath, params);
  if (!json.is_array()) {
    ::TypeMismatch(kEndpoint, "array", json);
  }
  std::vector<BatchJob> jobs;
  jobs.reserve(json.size());
  for (const auto& job_json : json.items()) {
    jobs.emplace_back(::Parse(kEndpoint, job_json.value()));
  }
  return jobs;
}

std::map<std::string, std::int32_t> Historical::MetadataListPublishers() {
  static const std::string kEndpoint = "ListPublishers";
  static const std::string kPath = ::BuildMetadataPath(".list_publishers");
  const nlohmann::json json = client_.GetJson(kPath, httplib::Params{});
  std::map<std::string, std::int32_t> publisher_to_pub_id;
  if (!json.is_object()) {
    throw ::TypeMismatch(kEndpoint, "object", json);
  }
  for (const auto& pair : json.items()) {
    if (!pair.value().is_number_integer()) {
      throw ::TypeMismatch(kEndpoint, "integer number", pair.key(),
                           pair.value());
    }
    publisher_to_pub_id.emplace(pair.key(), pair.value());
  }
  return publisher_to_pub_id;
}

std::vector<std::string> Historical::MetadataListDatasets() {
  return this->MetadataListDatasets({}, {});
}
std::vector<std::string> Historical::MetadataListDatasets(
    const std::string& start_date, const std::string& end_date) {
  static const std::string kEndpoint = "ListDatasets";
  static const std::string kPath = ::BuildMetadataPath(".list_datasets");
  httplib::Params params{};
  ::SetIfNotEmpty(&params, "start_date", start_date);
  ::SetIfNotEmpty(&params, "end_date", end_date);
  const nlohmann::json json = client_.GetJson(kPath, params);
  if (!json.is_array()) {
    throw ::TypeMismatch(kEndpoint, "array", json);
  }
  std::vector<std::string> datasets;
  datasets.reserve(json.size());
  for (const auto& item : json.items()) {
    if (!item.value().is_string()) {
      // `key()` in an array is the index
      throw ::TypeMismatch(kEndpoint, "string", item.key(), item.value());
    }
    datasets.emplace_back(item.value());
  }
  return datasets;
}

std::vector<databento::Schema> Historical::MetadataListSchemas(
    const std::string& dataset) {
  return this->MetadataListSchemas(dataset, {}, {});
}
std::vector<databento::Schema> Historical::MetadataListSchemas(
    const std::string& dataset, const std::string& start_date,
    const std::string& end_date) {
  static const std::string kEndpoint = "ListSchemas";
  static const std::string kPath = ::BuildMetadataPath(".list_schemas");
  httplib::Params params{{"dataset", dataset}};
  ::SetIfNotEmpty(&params, "start_date", start_date);
  ::SetIfNotEmpty(&params, "end_date", end_date);
  const nlohmann::json json = client_.GetJson(kPath, params);
  if (!json.is_array()) {
    throw ::TypeMismatch(kEndpoint, "array", json);
  }
  std::vector<Schema> schemas;
  schemas.reserve(json.size());
  for (const auto& item : json.items()) {
    if (!item.value().is_string()) {
      throw ::TypeMismatch(kEndpoint, "string", item.key(), item.value());
    }
    schemas.emplace_back(FromString<Schema>(item.value()));
  }
  return schemas;
}

static const std::string kListUnitPricesEndpoint = "ListUnitPrices";
static const std::string kListUnitPricesPath =
    ::BuildMetadataPath(".list_unit_prices");

std::map<databento::FeedMode, std::map<databento::Schema, double>>
Historical::MetadataListUnitPrices(const std::string& dataset) {
  const nlohmann::json json = client_.GetJson(
      kListUnitPricesPath, httplib::Params{{"dataset", dataset}});
  if (!json.is_object()) {
    throw ::TypeMismatch(kListUnitPricesEndpoint, "object", json);
  }
  std::map<FeedMode, std::map<Schema, double>> prices;
  for (const auto& mode_and_prices : json.items()) {
    if (!mode_and_prices.value().is_object()) {
      throw ::TypeMismatch(kListUnitPricesEndpoint, "object",
                           mode_and_prices.key(), mode_and_prices.value());
    }
    decltype(prices)::mapped_type schema_prices;
    for (const auto& schema_and_price : mode_and_prices.value().items()) {
      if (!schema_and_price.value().is_number()) {
        throw ::TypeMismatch(kListUnitPricesEndpoint, "nested number",
                             schema_and_price.key(), schema_and_price.value());
      }
      schema_prices.emplace(FromString<Schema>(schema_and_price.key()),
                            schema_and_price.value());
    }
    prices.emplace(FromString<FeedMode>(mode_and_prices.key()),
                   std::move(schema_prices));
  }
  return prices;
}

std::map<databento::Schema, double> Historical::MetadataListUnitPrices(
    const std::string& dataset, databento::FeedMode mode) {
  const std::string mode_str = ToString(mode);
  const nlohmann::json json = client_.GetJson(
      kListUnitPricesPath,
      httplib::Params{{"dataset", dataset}, {"mode", mode_str}});
  if (!json.is_object()) {
    throw ::TypeMismatch(kListUnitPricesEndpoint, "object", json);
  }
  const auto& json_map = ::CheckedAt(kListUnitPricesEndpoint, json, mode_str);
  std::map<Schema, double> prices;
  for (const auto& item : json_map.items()) {
    if (!item.value().is_number()) {
      throw ::TypeMismatch(kListUnitPricesEndpoint, "number", item.key(),
                           item.value());
    }
    prices.emplace(FromString<Schema>(item.key()), item.value());
  }
  return prices;
}

std::map<databento::FeedMode, double> Historical::MetadataListUnitPrices(
    const std::string& dataset, databento::Schema schema) {
  const std::string schema_str = ToString(schema);
  const nlohmann::json json = client_.GetJson(
      kListUnitPricesPath,
      httplib::Params{{"dataset", dataset}, {"schema", schema_str}});
  if (!json.is_object()) {
    throw ::TypeMismatch(kListUnitPricesEndpoint, "object", json);
  }
  std::map<FeedMode, double> prices;
  for (const auto& mode_and_prices : json.items()) {
    if (!mode_and_prices.value().is_object()) {
      throw ::TypeMismatch(kListUnitPricesEndpoint, "object",
                           mode_and_prices.key(), mode_and_prices.value());
    }
    const auto& price_json =
        ::CheckedAt("ListUnitPrices", mode_and_prices.value(), schema_str);
    if (!price_json.is_number()) {
      throw ::TypeMismatch(kListUnitPricesEndpoint, "number", price_json);
    }

    prices.emplace(FromString<FeedMode>(mode_and_prices.key()), price_json);
  }
  return prices;
}

double Historical::MetadataListUnitPrices(const std::string& dataset,
                                          FeedMode mode, Schema schema) {
  const nlohmann::json json = client_.GetJson(
      kListUnitPricesPath, httplib::Params{{"dataset", dataset},
                                           {"mode", ToString(mode)},
                                           {"schema", ToString(schema)}});
  if (!json.is_number()) {
    throw ::TypeMismatch("ListUnitPrices", "number", json);
  }
  return json;
}

std::size_t Historical::MetadataGetBillableSize(const std::string& dataset,
                                                const std::string& start,
                                                const std::string& end) {
  return this->MetadataGetBillableSize(dataset, start, end, {}, Schema::Trades,
                                       SType::Native, {});
}

std::size_t Historical::MetadataGetBillableSize(
    const std::string& dataset, const std::string& start,
    const std::string& end, const std::vector<std::string>& symbols,
    Schema schema, SType stype_in, std::size_t limit) {
  static const std::string kPath = ::BuildMetadataPath(".get_billable_size");
  httplib::Params params{{"dataset", dataset},
                         {"schema", ToString(schema)},
                         {"stype_in", ToString(stype_in)}};
  ::SetIfNotEmpty(&params, "start", start);
  ::SetIfNotEmpty(&params, "end", end);
  ::SetIfPositive(&params, "limit", limit);
  ::SetIfNotEmpty(&params, "symbols", symbols);
  const nlohmann::json json = client_.GetJson(kPath, params);
  if (!json.is_number_unsigned()) {
    throw ::TypeMismatch("GetBillableSize", "unsigned number", json);
  }
  return json;
}

double Historical::MetadataGetCost(const std::string& dataset,
                                   const std::string& start,
                                   const std::string& end) {
  return this->MetadataGetCost(dataset, start, end,
                               FeedMode::HistoricalStreaming, {},
                               Schema::Trades, SType::Native, {});
}

double Historical::MetadataGetCost(const std::string& dataset,
                                   const std::string& start,
                                   const std::string& end, FeedMode mode,
                                   const std::vector<std::string>& symbols,
                                   Schema schema, SType stype_in,
                                   std::size_t limit) {
  static const std::string kPath = ::BuildMetadataPath(".get_cost");
  httplib::Params params{{"dataset", dataset},
                         {"mode", ToString(mode)},
                         {"schema", ToString(schema)},
                         {"stype_in", ToString(stype_in)}};
  ::SetIfNotEmpty(&params, "start", start);
  ::SetIfNotEmpty(&params, "end", end);
  ::SetIfNotEmpty(&params, "symbols", symbols);
  ::SetIfPositive(&params, "limit", limit);
  const nlohmann::json json = client_.GetJson(kPath, params);
  if (!json.is_number()) {
    throw ::TypeMismatch("GetCost", "number", json);
  }
  return json;
}

databento::SymbologyResolution Historical::SymbologyResolve(
    const std::string& dataset, const std::vector<std::string>& symbols,
    SType stype_in, SType stype_out, const std::string& start_date,
    const std::string& end_date) {
  return this->SymbologyResolve(dataset, symbols, stype_in, stype_out,
                                start_date, end_date, {});
}

databento::SymbologyResolution Historical::SymbologyResolve(
    const std::string& dataset, const std::vector<std::string>& symbols,
    SType stype_in, SType stype_out, const std::string& start_date,
    const std::string& end_date, const std::string& default_value) {
  static const std::string kEndpoint = "SymbologyResolve";
  static const std::string kPath = ::BuildSymbologyPath(".resolve");
  httplib::Params params{{"dataset", dataset},
                         {"stype_in", ToString(stype_in)},
                         {"stype_out", ToString(stype_out)},
                         {"start_date", start_date},
                         {"end_date", end_date},
                         {"default_value", default_value}};
  ::SetIfNotEmpty(&params, "symbols", symbols);
  const nlohmann::json json = client_.GetJson(kPath, params);
  if (!json.is_object()) {
    throw ::TypeMismatch(kEndpoint, "object", json);
  }
  const auto& mappings_json = ::CheckedAt("SymbologyResolve", json, "result");
  const auto& partial_json = ::CheckedAt("SymbologyResolve", json, "partial");
  const auto& not_found_json =
      ::CheckedAt("SymbologyResolve", json, "not_found");
  SymbologyResolution res{};
  if (!mappings_json.is_object()) {
    throw ::TypeMismatch(kEndpoint, "mappings object", mappings_json);
  }
  res.mappings.reserve(mappings_json.size());
  for (const auto& mapping : mappings_json.items()) {
    const auto& mapping_json = mapping.value();
    if (!mapping_json.is_array()) {
      throw ::TypeMismatch(kEndpoint, "array", mapping.key(), mapping_json);
    }
    std::vector<MappingInterval> mapping_intervals;
    mapping_intervals.reserve(mapping_json.size());
    for (const auto& interval_json : mapping_json.items()) {
      mapping_intervals.emplace_back(MappingInterval{
          ::CheckedAt(kEndpoint, interval_json.value(), "d0"),
          ::CheckedAt(kEndpoint, interval_json.value(), "d1"),
          ::CheckedAt(kEndpoint, interval_json.value(), "s"),
      });
    }
    res.mappings.emplace(mapping.key(), std::move(mapping_intervals));
  }
  if (!partial_json.is_array()) {
    throw ::TypeMismatch(kEndpoint, "partial array", partial_json);
  }
  res.partial.reserve(partial_json.size());
  for (const auto& symbol : partial_json.items()) {
    if (!symbol.value().is_string()) {
      throw ::TypeMismatch(kEndpoint, "nested string", symbol.key(),
                           symbol.value());
    }
    res.partial.emplace_back(symbol.value());
  }
  if (!not_found_json.is_array()) {
    throw ::TypeMismatch(kEndpoint, "not_found array", not_found_json);
  }
  res.not_found.reserve(not_found_json.size());
  for (const auto& symbol : not_found_json.items()) {
    if (!symbol.value().is_string()) {
      throw ::TypeMismatch(kEndpoint, "nested string", symbol.key(),
                           symbol.value());
    }
    res.not_found.emplace_back(symbol.value());
  }
  return res;
}

void Historical::TimeseriesStream(
    const std::string& dataset, const std::vector<std::string>& symbols,
    Schema schema, std::chrono::system_clock::time_point start,
    std::chrono::system_clock::time_point end, SType stype_in, SType stype_out,
    std::size_t limit, const MetadataCallback& metadata_callback,
    const RecordCallback& callback) {
  static const std::string kEndpoint = "TimeseriesStream";
  static const std::string kPath = ::BuildSymbologyPath(".stream");
  httplib::Params params{{"dataset", dataset},
                         {"schema", ToString(schema)},
                         {"stype_in", ToString(stype_in)},
                         {"stype_out", ToString(stype_out)},
                         {"start", ToString(start)},
                         {"end", ToString(end)}};
  ::SetIfNotEmpty(&params, "symbols", symbols);
  ::SetIfPositive(&params, "limit", limit);

  std::atomic<bool> should_continue{true};
  DbzParser dbz_parser;
  // no initialized lambda captures until C++14
  std::thread stream{[this, &dbz_parser, &params, &should_continue] {
    this->client_.GetRawStream(
        kPath, params,
        [&dbz_parser, &should_continue](const char* data, std::size_t length) {
          dbz_parser.PassBytes(reinterpret_cast<const std::uint8_t*>(data),
                               length);
          return should_continue.load();
        });
    dbz_parser.EndInput();
  }};
  Metadata metadata = dbz_parser.ParseMetadata();
  const auto record_count = metadata.record_count;
  metadata_callback(std::move(metadata));
  for (auto i = 0UL; i < record_count; ++i) {
    should_continue = callback(dbz_parser.ParseRecord()) == KeepGoing::Continue;
  }
  stream.join();
}

HistoricalBuilder& HistoricalBuilder::keyFromEnv() {
  char const* env_key = std::getenv("DATABENTO_API_KEY");
  if (env_key == nullptr) {
    throw std::runtime_error{
        "Expected environment variable DATABENTO_API_KEY to be set"};
  }
  return this->key(env_key);
}

HistoricalBuilder& HistoricalBuilder::key(std::string key) {
  key_ = std::move(key);
  return *this;
}

HistoricalBuilder& HistoricalBuilder::gateway(HistoricalGateway gateway) {
  gateway_ = gateway;
  return *this;
}

Historical HistoricalBuilder::Build() {
  if (key_.empty()) {
    throw std::logic_error{"key is unset"};
  }
  return Historical{std::move(key_), gateway_};
}
