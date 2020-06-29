// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "server/delivery/hybrid_request/CreateHybridCollectionRequest.h"
#include "db/Utils.h"
#include "server/DBWrapper.h"
#include "server/ValidationUtil.h"
#include "server/delivery/request/BaseRequest.h"
#include "server/web_impl/Constants.h"
#include "utils/Log.h"
#include "utils/TimeRecorder.h"

#include <fiu-local.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace milvus {
namespace server {

CreateHybridCollectionRequest::CreateHybridCollectionRequest(
    const std::shared_ptr<milvus::server::Context>& context, const std::string& collection_name,
    std::unordered_map<std::string, engine::meta::hybrid::DataType>& field_types,
    std::unordered_map<std::string, milvus::json>& field_index_params,
    std::unordered_map<std::string, std::string>& field_params, milvus::json& extra_params)
    : BaseRequest(context, BaseRequest::kCreateHybridCollection),
      collection_name_(collection_name),
      field_types_(field_types),
      field_index_params_(field_index_params),
      field_params_(field_params) {
}

BaseRequestPtr
CreateHybridCollectionRequest::Create(const std::shared_ptr<milvus::server::Context>& context,
                                      const std::string& collection_name,
                                      std::unordered_map<std::string, engine::meta::hybrid::DataType>& field_types,
                                      std::unordered_map<std::string, milvus::json>& field_index_params,
                                      std::unordered_map<std::string, std::string>& field_params,
                                      milvus::json& extra_params) {
    return std::shared_ptr<BaseRequest>(new CreateHybridCollectionRequest(
        context, collection_name, field_types, field_index_params, field_params, extra_params));
}

Status
CreateHybridCollectionRequest::OnExecute() {
    std::string hdr = "CreateCollectionRequest(collection=" + collection_name_ + ")";
    TimeRecorderAuto rc(hdr);

    try {
        // step 1: check arguments
        auto status = ValidateCollectionName(collection_name_);
        fiu_do_on("CreateHybridCollectionRequest.OnExecute.invalid_collection_name",
                  status = Status(milvus::SERVER_UNEXPECTED_ERROR, ""));
        if (!status.ok()) {
            return status;
        }

        rc.RecordSection("check validation");

        // step 2: construct collection schema and vector schema
        engine::meta::CollectionSchema collection_info;
        engine::meta::hybrid::FieldsSchema fields_schema;

        uint16_t dimension = 0;
        milvus::json vector_param;
        for (auto& field_type : field_types_) {
            engine::meta::hybrid::FieldSchema schema;
            auto field_name = field_type.first;
            auto index_params = field_index_params_.at(field_name);
            schema.collection_id_ = collection_name_;
            schema.field_name_ = field_name;
            schema.field_type_ = (int32_t)field_type.second;
            if (index_params.contains("name")) {
                schema.index_name_ = index_params["name"];
            }
            schema.index_param_ = index_params.dump();

            auto field_param = field_params_.at(field_name);
            schema.field_params_ = field_param;
            fields_schema.fields_schema_.emplace_back(schema);

            if (field_type.second == engine::meta::hybrid::DataType::FLOAT_VECTOR ||
                field_type.second == engine::meta::hybrid::DataType::BINARY_VECTOR) {
                vector_param = milvus::json::parse(field_param);
                if (vector_param.contains("dimension")) {
                    dimension = vector_param["dimension"].get<uint16_t>();
                }
            }
        }

        collection_info.collection_id_ = collection_name_;
        collection_info.dimension_ = dimension;
        if (extra_params_.contains("segment_size")) {
            collection_info.index_file_size_ = extra_params_["segment_size"].get<int64_t>();
        }

        if (vector_param.contains("metric_type")) {
            int32_t metric_type = (int32_t)milvus::engine::s_map_metric_type.at(vector_param["metric_type"]);
            collection_info.metric_type_ = metric_type;
        }

        if (vector_param.contains("index_type")) {
            int32_t engine_type = (int32_t)milvus::engine::s_map_engine_type.at(vector_param["index_type"]);
            collection_info.engine_type_ = engine_type;
        }

        // step 3: create collection
        status = DBWrapper::DB()->CreateHybridCollection(collection_info, fields_schema);
        fiu_do_on("CreateHybridCollectionRequest.OnExecute.invalid_db_execute",
                  status = Status(milvus::SERVER_UNEXPECTED_ERROR, ""));
        if (!status.ok()) {
            // collection could exist
            if (status.code() == DB_ALREADY_EXIST) {
                return Status(SERVER_INVALID_COLLECTION_NAME, status.message());
            }
            return status;
        }
    } catch (std::exception& ex) {
        return Status(SERVER_UNEXPECTED_ERROR, ex.what());
    }

    return Status::OK();
}

}  // namespace server
}  // namespace milvus