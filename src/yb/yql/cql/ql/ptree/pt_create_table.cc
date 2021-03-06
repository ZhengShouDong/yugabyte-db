//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//
// Treenode definitions for CREATE TABLE statements.
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/ptree/pt_create_table.h"
#include "yb/yql/cql/ql/ptree/sem_context.h"

DECLARE_bool(use_cassandra_authentication);

namespace yb {
namespace ql {

using std::shared_ptr;
using std::to_string;
using client::YBColumnSchema;

//--------------------------------------------------------------------------------------------------

PTCreateTable::PTCreateTable(MemoryContext *memctx,
                             YBLocation::SharedPtr loc,
                             const PTQualifiedName::SharedPtr& name,
                             const PTListNode::SharedPtr& elements,
                             bool create_if_not_exists,
                             const PTTablePropertyListNode::SharedPtr& table_properties)
    : TreeNode(memctx, loc),
      relation_(name),
      elements_(elements),
      columns_(memctx),
      primary_columns_(memctx),
      hash_columns_(memctx),
      create_if_not_exists_(create_if_not_exists),
      contain_counters_(false),
      table_properties_(table_properties) {
}

PTCreateTable::~PTCreateTable() {
}

CHECKED_STATUS PTCreateTable::Analyze(SemContext *sem_context) {
  SemState sem_state(sem_context);

  if (FLAGS_use_cassandra_authentication) {
    RETURN_NOT_OK(sem_context->CheckHasKeyspacePermission(loc(), PermissionType::CREATE_PERMISSION,
        yb_table_name().namespace_name()));
  }

  // Processing table name.
  RETURN_NOT_OK(relation_->AnalyzeName(sem_context, OBJECT_TABLE));

  // Save context state, and set "this" as current column in the context.
  SymbolEntry cached_entry = *sem_context->current_processing_id();
  sem_context->set_current_create_table_stmt(this);

  // Processing table elements.
  // - First, process all column definitions to collect all symbols.
  // - Process all other elements afterward.
  sem_state.set_processing_column_definition(true);
  RETURN_NOT_OK(elements_->Analyze(sem_context));
  sem_state.set_processing_column_definition(false);
  RETURN_NOT_OK(elements_->Analyze(sem_context));

  // Move the all partition and primary key columns from columns list to appropriate list.
  int32_t order = 0;
  MCList<PTColumnDefinition *>::iterator iter = columns_.begin();
  while (iter != columns_.end()) {
    PTColumnDefinition *coldef = *iter;
    coldef->set_order(order++);

    // Remove a column from regular column list if it's already in hash or primary list.
    if (coldef->is_hash_key()) {
      if (coldef->is_static()) {
        return sem_context->Error(coldef, "Hash column cannot be static",
                                  ErrorCode::INVALID_TABLE_DEFINITION);
      }
      iter = columns_.erase(iter);
    } else if (coldef->is_primary_key()) {
      if (coldef->is_static()) {
        return sem_context->Error(coldef, "Primary key column cannot be static",
                                  ErrorCode::INVALID_TABLE_DEFINITION);
      }
      iter = columns_.erase(iter);
    } else {
      if (contain_counters_ != coldef->is_counter()) {
        return sem_context->Error(coldef,
                                  "Table cannot contain both counter and non-counter columns",
                                  ErrorCode::INVALID_TABLE_DEFINITION);
      }
      iter++;
    }
  }

  // When partition key is not defined, the first column is assumed to be the partition key.
  if (hash_columns_.empty()) {
    if (primary_columns_.empty()) {
      return sem_context->Error(elements_, ErrorCode::MISSING_PRIMARY_KEY);
    }
    RETURN_NOT_OK(AppendHashColumn(sem_context, primary_columns_.front()));
    primary_columns_.pop_front();
  }

  // After primary key columns are fully analyzed, return error if the table contains no range
  // (primary) column but a static column. With no range column, every non-key column is static
  // by nature and Cassandra raises error in such a case.
  if (primary_columns_.empty()) {
    for (const auto& column : columns_) {
      if (column->is_static()) {
        return sem_context->Error(column,
                                  "Static column not allowed in a table without a range column",
                                  ErrorCode::INVALID_TABLE_DEFINITION);
      }
    }
  }

  if (table_properties_ != nullptr) {
    // Process table properties.
    RETURN_NOT_OK(table_properties_->Analyze(sem_context));
  }

  // Restore the context value as we are done with this table.
  sem_context->set_current_processing_id(cached_entry);
  if (VLOG_IS_ON(3)) {
    PrintSemanticAnalysisResult(sem_context);
  }

  return Status::OK();
}

namespace {

bool ColumnExists(const MCList<PTColumnDefinition *>& columns, const PTColumnDefinition* column) {
  return std::find(columns.begin(), columns.end(), column) != columns.end();
}

} // namespace

CHECKED_STATUS PTCreateTable::AppendColumn(SemContext *sem_context,
                                           PTColumnDefinition *column,
                                           const bool check_duplicate) {
  RETURN_NOT_OK(CheckType(sem_context, column->datatype()));
  if (check_duplicate && ColumnExists(columns_, column)) {
    return sem_context->Error(column, ErrorCode::DUPLICATE_COLUMN);
  }
  columns_.push_back(column);

  if (column->is_counter()) {
    contain_counters_ = true;
  }
  return Status::OK();
}

CHECKED_STATUS PTCreateTable::AppendPrimaryColumn(SemContext *sem_context,
                                                  PTColumnDefinition *column,
                                                  const bool check_duplicate) {
  RETURN_NOT_OK(CheckPrimaryType(sem_context, column->datatype()));
  if (check_duplicate && ColumnExists(primary_columns_, column)) {
    return sem_context->Error(column, ErrorCode::DUPLICATE_COLUMN);
  }
  column->set_is_primary_key();
  primary_columns_.push_back(column);
  return Status::OK();
}

CHECKED_STATUS PTCreateTable::AppendHashColumn(SemContext *sem_context,
                                               PTColumnDefinition *column,
                                               const bool check_duplicate) {
  RETURN_NOT_OK(CheckPrimaryType(sem_context, column->datatype()));
  if (check_duplicate && ColumnExists(hash_columns_, column)) {
    return sem_context->Error(column, ErrorCode::DUPLICATE_COLUMN);
  }
  column->set_is_hash_key();
  hash_columns_.push_back(column);
  return Status::OK();
}

CHECKED_STATUS PTCreateTable::CheckPrimaryType(SemContext *sem_context,
                                               const PTBaseType::SharedPtr& datatype) {
  RETURN_NOT_OK(CheckType(sem_context, datatype));
  if (!QLType::IsValidPrimaryType(datatype->ql_type()->main())) {
    return sem_context->Error(datatype, ErrorCode::INVALID_PRIMARY_COLUMN_TYPE);
  }
  return Status::OK();
}

CHECKED_STATUS PTCreateTable::CheckType(SemContext *sem_context,
                                        const PTBaseType::SharedPtr& datatype) {
  // Although simple datatypes don't need further checking, complex datatypes such as collections,
  // tuples, and user-defined datatypes need to be analyzed because they have members.
  RETURN_NOT_OK(datatype->Analyze(sem_context));

  return Status::OK();
}

void PTCreateTable::PrintSemanticAnalysisResult(SemContext *sem_context) {
  MCString sem_output("\tTable ", sem_context->PTempMem());
  sem_output += yb_table_name().ToString().c_str();
  sem_output += "(";

  MCList<PTColumnDefinition *> columns(sem_context->PTempMem());
  for (auto column : hash_columns_) {
    columns.push_back(column);
  }
  for (auto column : primary_columns_) {
    columns.push_back(column);
  }
  for (auto column : columns_) {
    columns.push_back(column);
  }

  bool is_first = true;
  for (auto column : columns) {
    if (is_first) {
      is_first = false;
    } else {
      sem_output += ", ";
    }
    sem_output += column->yb_name();
    if (column->is_hash_key()) {
      sem_output += " <Hash key, Type = ";
    } else if (column->is_primary_key()) {
      sem_output += " <Primary key, ";
      switch (column->sorting_type()) {
        case ColumnSchema::SortingType::kNotSpecified: sem_output += "None"; break;
        case ColumnSchema::SortingType::kAscending: sem_output += "Asc"; break;
        case ColumnSchema::SortingType::kDescending: sem_output += "Desc"; break;
      }
      sem_output += ", Type = ";
    } else {
      sem_output += " <Type = ";
    }
    sem_output += column->ql_type()->ToString().c_str();
    sem_output += ">";
  }

  sem_output += ")";
  VLOG(3) << "SEMANTIC ANALYSIS RESULT (" << *loc_ << "):\n" << sem_output;
}

CHECKED_STATUS PTCreateTable::ToTableProperties(TableProperties *table_properties) const {
  if (table_properties_ != nullptr) {
    for (PTTableProperty::SharedPtr table_property : table_properties_->node_list()) {
      RETURN_NOT_OK(table_property->SetTableProperty(table_properties));
    }
  }

  table_properties->SetContainCounters(contain_counters_);
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

PTColumnDefinition::PTColumnDefinition(MemoryContext *memctx,
                                       YBLocation::SharedPtr loc,
                                       const MCSharedPtr<MCString>& name,
                                       const PTBaseType::SharedPtr& datatype,
                                       const PTListNode::SharedPtr& qualifiers)
    : TreeNode(memctx, loc),
      name_(name),
      datatype_(datatype),
      qualifiers_(qualifiers),
      is_primary_key_(false),
      is_hash_key_(false),
      is_static_(false),
      order_(-1),
      sorting_type_(ColumnSchema::SortingType::kNotSpecified) {
}

PTColumnDefinition::~PTColumnDefinition() {
}

CHECKED_STATUS PTColumnDefinition::Analyze(SemContext *sem_context) {
  if (!sem_context->processing_column_definition()) {
    return Status::OK();
  }

  // Save context state, and set "this" as current column in the context.
  SymbolEntry cached_entry = *sem_context->current_processing_id();
  sem_context->set_current_column(this);

  // Analyze column qualifiers.
  RETURN_NOT_OK(sem_context->MapSymbol(*name_, this));
  if (qualifiers_ != nullptr) {
    RETURN_NOT_OK(qualifiers_->Analyze(sem_context));
  }

  // Add the analyzed column to table.
  PTCreateTable *table = sem_context->current_create_table_stmt();
  RETURN_NOT_OK(table->AppendColumn(sem_context, this));

  // Restore the context value as we are done with this colummn.
  sem_context->set_current_processing_id(cached_entry);
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

PTPrimaryKey::PTPrimaryKey(MemoryContext *memctx,
                           YBLocation::SharedPtr loc,
                           const PTListNode::SharedPtr& columns)
    : PTConstraint(memctx, loc),
      columns_(columns) {
}

PTPrimaryKey::~PTPrimaryKey() {
}

CHECKED_STATUS PTPrimaryKey::Analyze(SemContext *sem_context) {
  if (sem_context->processing_column_definition() != is_column_element()) {
    return Status::OK();
  }

  // Check if primary key is defined more than one time.
  PTCreateTable *table = sem_context->current_create_table_stmt();
  if (table->primary_columns().size() > 0 || table->hash_columns().size() > 0) {
    return sem_context->Error(this, "Too many primary key", ErrorCode::INVALID_TABLE_DEFINITION);
  }

  if (columns_ == nullptr) {
    // Decorate the current processing name node as this is a column constraint.
    PTColumnDefinition *column = sem_context->current_column();
    RETURN_NOT_OK(table->AppendHashColumn(sem_context, column));
  } else {
    // Decorate all name node of this key as this is a table constraint.
    TreeNodeOperator<SemContext, PTName> func = &PTName::SetupPrimaryKey;
    TreeNodeOperator<SemContext, PTName> nested_func = &PTName::SetupHashAndPrimaryKey;
    RETURN_NOT_OK((columns_->Apply<SemContext, PTName>(sem_context, func, 1, 1, nested_func)));
  }
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS PTStatic::Analyze(SemContext *sem_context) {
  // Decorate the current column as static.
  PTColumnDefinition *column = sem_context->current_column();
  column->set_is_static();
  return Status::OK();
}

}  // namespace ql
}  // namespace yb
