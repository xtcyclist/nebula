/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "util/IndexUtil.h"

namespace nebula {
namespace graph {

Status IndexUtil::validateColumns(const std::vector<std::string>& fields) {
    std::unordered_set<std::string> fieldSet(fields.begin(), fields.end());
    if (fieldSet.size() != fields.size()) {
        return Status::Error("Found duplicate column field");
    }

    if (fields.empty()) {
        return Status::Error("Column is empty");
    }

    return Status::OK();
}

StatusOr<DataSet> IndexUtil::toDescIndex(const meta::cpp2::IndexItem &indexItem) {
    DataSet dataSet({"Field", "Type"});
    for (auto &col : indexItem.get_fields()) {
        Row row;
        row.values.emplace_back(Value(col.get_name()));
        row.values.emplace_back(SchemaUtil::typeToString(col));
        dataSet.emplace_back(std::move(row));
    }
    return dataSet;
}

StatusOr<DataSet> IndexUtil::toShowCreateIndex(bool isTagIndex,
                                               const std::string &indexName,
                                               const meta::cpp2::IndexItem &indexItem) {
    DataSet dataSet;
    std::string createStr;
    createStr.reserve(1024);
    std::string schemaName = indexItem.get_schema_name();
    if (isTagIndex) {
        dataSet.colNames = {"Tag Index Name", "Create Tag Index"};
        createStr = "CREATE TAG INDEX `" + indexName +  "` ON `" + schemaName + "` (\n";
    } else {
        dataSet.colNames = {"Edge Index Name", "Create Edge Index"};
        createStr = "CREATE EDGE INDEX `" + indexName + "` ON `" + schemaName + "` (\n";
    }
    Row row;
    row.emplace_back(indexName);
    for (auto &col : indexItem.get_fields()) {
        createStr += " `" + col.get_name();
        const auto &type = col.get_type();
        if (type.__isset.type_length) {
            createStr += "(" + std::to_string(*type.get_type_length()) + ")";
        }
        createStr += "`,\n";
    }
    if (!indexItem.fields.empty()) {
        createStr.resize(createStr.size() -2);
        createStr += "\n";
    }
    createStr += ")";
    row.emplace_back(std::move(createStr));
    dataSet.rows.emplace_back(std::move(row));
    return dataSet;
}

}  // namespace graph
}  // namespace nebula