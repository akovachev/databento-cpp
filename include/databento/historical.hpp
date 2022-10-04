#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "databento/batch.hpp"
#include "databento/enums.hpp"
#include "databento/http_client.hpp"
#include "databento/metadata.hpp"
#include "databento/symbology.hpp"
#include "databento/timeseries.hpp"

namespace databento {
class Historical {
  const std::string key_;
  const std::string gateway_;
  HttpClient client_;

 public:
  Historical(std::string key, HistoricalGateway gateway);
  // Primarily for unit tests
  Historical(std::string key, std::string gateway, std::uint16_t port);

  // Getters

  const std::string& key() const { return key_; };
  const std::string& gateway() const { return gateway_; }

  // Batch API

  BatchJob BatchSubmitJob(const std::string& dataset, Schema schema,
                          const std::vector<std::string>& symbols,
                          const std::string& start, const std::string& end);
  BatchJob BatchSubmitJob(const std::string& dataset, Schema schema,
                          const std::vector<std::string>& symbols,
                          const std::string& start, const std::string& end,
                          // FIXME: encoding and compression?
                          DurationInterval split_duration,
                          std::size_t split_size, Packaging packaging,
                          Delivery delivery, SType stype_in, SType stype_out,
                          std::size_t limit);
  std::vector<BatchJob> BatchListJobs();
  std::vector<BatchJob> BatchListJobs(const std::vector<BatchState>& states,
                                      const std::string& since);

  // Metadata API
  // list_fields, list_encodings, and list_compressions don't seem useful in C++

  // Retrievs a mapping of publisher name to publisher ID.
  std::map<std::string, std::int32_t> MetadataListPublishers();
  std::vector<std::string> MetadataListDatasets();
  std::vector<std::string> MetadataListDatasets(const std::string& start_date,
                                                const std::string& end_date);
  std::vector<Schema> MetadataListSchemas(const std::string& dataset);
  std::vector<Schema> MetadataListSchemas(const std::string& dataset,
                                          const std::string& start_date,
                                          const std::string& end_date);
  PriceByFeedModeAndSchema MetadataListUnitPrices(const std::string& dataset);
  PriceBySchema MetadataListUnitPrices(const std::string& dataset,
                                       FeedMode mode);
  PriceByFeedMode MetadataListUnitPrices(const std::string& dataset,
                                         Schema schema);
  double MetadataListUnitPrices(const std::string& dataset, FeedMode mode,
                                Schema schema);
  std::size_t MetadataGetBillableSize(const std::string& dataset,
                                      const std::string& start,
                                      const std::string& end);
  std::size_t MetadataGetBillableSize(const std::string& dataset,
                                      const std::string& start,
                                      const std::string& end,
                                      const std::vector<std::string>& symbols,
                                      Schema schema, SType stype_in,
                                      std::size_t limit);
  double MetadataGetCost(const std::string& dataset, const std::string& start,
                         const std::string& end);
  double MetadataGetCost(const std::string& dataset, const std::string& start,
                         const std::string& end, FeedMode mode,
                         const std::vector<std::string>& symbols, Schema schema,
                         SType stype_in, std::size_t limit);

  // Symbology API
  SymbologyResolution SymbologyResolve(const std::string& dataset,
                                       const std::vector<std::string>& symbols,
                                       SType stype_in, SType stype_out,
                                       const std::string& start_date,
                                       const std::string& end_date);
  SymbologyResolution SymbologyResolve(const std::string& dataset,
                                       const std::vector<std::string>& symbols,
                                       SType stype_in, SType stype_out,
                                       const std::string& start_date,
                                       const std::string& end_date,
                                       const std::string& default_value);

  // Timeseries API
  void TimeseriesStream(const std::string& dataset,
                        const std::vector<std::string>& symbols, Schema schema,
                        std::chrono::system_clock::time_point start,
                        std::chrono::system_clock::time_point end,
                        SType stype_in, SType stype_out, std::size_t limit,
                        const MetadataCallback& metadata_callback,
                        const RecordCallback& callback);
};

class HistoricalBuilder {
  std::string key_;
  HistoricalGateway gateway_{HistoricalGateway::Nearest};

 public:
  HistoricalBuilder() = default;

  // Sets `key_` based on the environment variable DATABENTO_API_KEY.
  //
  // NOTE: This is not thread-safe if `std::setenv` is used elsewhere in the
  // program.
  HistoricalBuilder& keyFromEnv();
  HistoricalBuilder& key(std::string key);
  HistoricalBuilder& gateway(HistoricalGateway gateway);
  Historical Build();
};
}  // namespace databento
