//
// Copyright 2019 Google LLC
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
//

#include "zetasql/public/simple_catalog.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetasql/base/logging.h"
#include "zetasql/proto/simple_catalog.pb.h"
#include "zetasql/public/catalog.h"
#include "zetasql/public/catalog_helper.h"
#include "zetasql/public/constant.h"
#include "zetasql/public/procedure.h"
#include "zetasql/public/simple_connection.pb.h"
#include "zetasql/public/simple_constant.pb.h"
#include "zetasql/public/simple_model.pb.h"
#include "zetasql/public/simple_table.pb.h"
#include "zetasql/public/sql_view.h"
#include "zetasql/public/strings.h"
#include "zetasql/public/table_valued_function.h"
#include "zetasql/public/types/annotation.h"
#include "zetasql/public/types/type_deserializer.h"
#include "zetasql/base/case.h"
#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "zetasql/base/map_util.h"
#include "zetasql/base/source_location.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_builder.h"
#include "zetasql/base/status_macros.h"

namespace zetasql {

SimpleCatalog::SimpleCatalog(absl::string_view name, TypeFactory* type_factory)
    : name_(name), type_factory_(type_factory) {}

absl::Status SimpleCatalog::GetTable(const std::string& name,
                                     const Table** table,
                                     const FindOptions& options) {
  absl::ReaderMutexLock l(&mutex_);
  *table = zetasql_base::FindPtrOrNull(tables_, absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetModel(const std::string& name,
                                     const Model** model,
                                     const FindOptions& options) {
  absl::ReaderMutexLock l(&mutex_);
  *model = zetasql_base::FindPtrOrNull(models_, absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetConnection(const std::string& name,
                                          const Connection** connection,
                                          const FindOptions& options) {
  absl::ReaderMutexLock l(&mutex_);
  *connection = zetasql_base::FindPtrOrNull(connections_, absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetSequence(const std::string& name,
                                        const Sequence** sequence,
                                        const FindOptions& options) {
  absl::ReaderMutexLock l(&mutex_);
  *sequence = zetasql_base::FindPtrOrNull(sequences_, absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetFunction(const std::string& name,
                                        const Function** function,
                                        const FindOptions& options) {
  absl::ReaderMutexLock l(&mutex_);
  *function = zetasql_base::FindPtrOrNull(functions_, absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetTableValuedFunction(
    const std::string& name, const TableValuedFunction** function,
    const FindOptions& options) {
  absl::ReaderMutexLock l(&mutex_);
  *function =
      zetasql_base::FindPtrOrNull(table_valued_functions_, absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetProcedure(const std::string& name,
                                         const Procedure** procedure,
                                         const FindOptions& options) {
  absl::ReaderMutexLock l(&mutex_);
  *procedure = zetasql_base::FindPtrOrNull(procedures_, absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetType(const std::string& name, const Type** type,
                                    const FindOptions& options) {
  const google::protobuf::DescriptorPool* pool;
  {
    absl::ReaderMutexLock l(&mutex_);
    // Types contained in types_ have case-insensitive names, so we lowercase
    // the name as is done in AddType.
    *type = zetasql_base::FindPtrOrNull(types_, absl::AsciiStrToLower(name));
    if (*type != nullptr) {
      return absl::OkStatus();
    }
    // Avoid holding the mutex while calling descriptor_pool_ methods.
    // descriptor_pool_ is const once it has been set.
    pool = descriptor_pool_;
  }

  if (pool != nullptr) {
    const google::protobuf::Descriptor* descriptor = pool->FindMessageTypeByName(name);
    if (descriptor != nullptr) {
      return type_factory()->MakeProtoType(descriptor, type);
    }
    const google::protobuf::EnumDescriptor* enum_descriptor =
        pool->FindEnumTypeByName(name);
    if (enum_descriptor != nullptr) {
      return type_factory()->MakeEnumType(enum_descriptor, type);
    }
  }

  ABSL_DCHECK(*type == nullptr);
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetCatalog(const std::string& name,
                                       Catalog** catalog,
                                       const FindOptions& options) {
  absl::ReaderMutexLock l(&mutex_);
  *catalog = zetasql_base::FindPtrOrNull(catalogs_, absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetConstant(const std::string& name,
                                        const Constant** constant,
                                        const FindOptions& options) {
  absl::ReaderMutexLock l(&mutex_);
  *constant = zetasql_base::FindPtrOrNull(constants_, absl::AsciiStrToLower(name));
  return absl::OkStatus();
}

std::string SimpleCatalog::SuggestTable(
    const absl::Span<const std::string>& mistyped_path) {
  if (mistyped_path.empty()) {
    // Nothing to suggest here.
    return "";
  }

  const std::string& name = mistyped_path.front();
  if (mistyped_path.length() > 1) {
    Catalog* catalog = nullptr;
    if (GetCatalog(name, &catalog).ok() && catalog != nullptr) {
      absl::Span<const std::string> mistyped_path_suffix =
          mistyped_path.subspan(1, mistyped_path.length() - 1);
      const std::string closest_name =
          catalog->SuggestTable(mistyped_path_suffix);
      if (!closest_name.empty()) {
        return absl::StrCat(catalog->FullName(), ".", closest_name);
      }
    }
  } else {
    const FindOptions& find_options = FindOptions();
    const Table* table = nullptr;
    if (FindTable({name}, &table, find_options).ok()) {
      return table->Name();
    }

    std::vector<Catalog*> sub_catalogs = catalogs();
    std::string closest_name;
    for (int i = 0; i < sub_catalogs.size(); ++i) {
      if ((sub_catalogs[i]->FindTable({name}, &table, find_options).ok())) {
        const std::string result =
            absl::StrCat(sub_catalogs[i]->FullName(), ".", table->Name());
        // We choose the name which occurs lexicographically first to keep the
        // result deterministic and independent of the order of sub-catalogs.
        if (closest_name.empty() || closest_name.compare(result) > 0) {
          closest_name = result;
        }
      }
    }
    if (!closest_name.empty()) {
      return closest_name;
    }
    closest_name = ClosestName(absl::AsciiStrToLower(name), table_names());
    if (!closest_name.empty()) {
      if (FindTable({closest_name}, &table).ok()) {
        return table->Name();
      }
    }
  }

  // No suggestion obtained.
  return "";
}

std::string SimpleCatalog::SuggestFunctionOrTableValuedFunction(
    bool is_table_valued_function,
    absl::Span<const std::string> mistyped_path) {
  if (mistyped_path.empty()) {
    // Nothing to suggest here.
    return "";
  }

  const std::string& name = mistyped_path.front();
  if (mistyped_path.length() > 1) {
    Catalog* catalog = nullptr;
    if (GetCatalog(name, &catalog).ok() && catalog != nullptr) {
      absl::Span<const std::string> path_suffix =
          mistyped_path.subspan(1, mistyped_path.length() - 1);
      const std::string closest_name =
          is_table_valued_function
              ? catalog->SuggestTableValuedFunction(path_suffix)
              : catalog->SuggestFunction(path_suffix);
      if (!closest_name.empty()) {
        return absl::StrCat(catalog->FullName(), ".", closest_name);
      }
    }
  } else {
    const std::string closest_name =
        ClosestName(absl::AsciiStrToLower(name),
                    is_table_valued_function ? table_valued_function_names()
                                             : function_names());
    if (!closest_name.empty()) {
      return closest_name;
    }

    // TODO: Add support for suggesting function names from nested
    // catalogs, once accessing functions in sub_catalogs is supported in
    // zetasql.
    // TODO: We should verify that suggested function has a valid
    // signature where it is suggested. Maybe we should get a list of all
    // possible names under allowed ~20% edit distance and return the one which
    // has a matching signature.
  }

  // No suggestion obtained.
  return "";
}

std::string SimpleCatalog::SuggestFunction(
    const absl::Span<const std::string>& mistyped_path) {
  return SuggestFunctionOrTableValuedFunction(
      /*is_table_valued_function=*/false, mistyped_path);
}

std::string SimpleCatalog::SuggestTableValuedFunction(
    const absl::Span<const std::string>& mistyped_path) {
  return SuggestFunctionOrTableValuedFunction(
      /*is_table_valued_function=*/true, mistyped_path);
}

std::string SimpleCatalog::SuggestConstant(
    const absl::Span<const std::string>& mistyped_path) {
  if (mistyped_path.empty()) {
    // Nothing to suggest here.
    return "";
  }

  const std::string& name = mistyped_path.front();
  if (mistyped_path.length() > 1) {
    Catalog* catalog = nullptr;
    if (GetCatalog(name, &catalog).ok() && catalog != nullptr) {
      const std::string closest_name = catalog->SuggestConstant(
          mistyped_path.subspan(1, mistyped_path.length() - 1));
      if (!closest_name.empty()) {
        return absl::StrCat(ToIdentifierLiteral(catalog->FullName()), ".",
                            closest_name);
      }
    }
  } else {
    const std::string closest_name =
        ClosestName(absl::AsciiStrToLower(name), constant_names());
    if (!closest_name.empty()) {
      // A suggestion was found based on lower-case string comparison. Retrieve
      // the suggested Constant and return its original name.
      const Constant* constant = nullptr;
      if (FindConstant({closest_name}, &constant).ok()) {
        ABSL_DCHECK_NE(constant, nullptr) << closest_name;
        return ToIdentifierLiteral(constant->Name());
      }
    }
  }

  // No suggestion obtained.
  return "";
}

void SimpleCatalog::AddTable(absl::string_view name, const Table* table) {
  absl::MutexLock l(&mutex_);
  const std::string canonical_name = absl::AsciiStrToLower(name);
  zetasql_base::InsertOrDie(&global_names_, canonical_name);
  zetasql_base::InsertOrDie(&tables_, canonical_name, table);
}

void SimpleCatalog::AddModel(absl::string_view name, const Model* model) {
  absl::MutexLock l(&mutex_);
  zetasql_base::InsertOrDie(&models_, absl::AsciiStrToLower(name), model);
}

void SimpleCatalog::AddConnection(const std::string& name,
                                  const Connection* connection) {
  absl::MutexLock l(&mutex_);
  zetasql_base::InsertOrDie(&connections_, absl::AsciiStrToLower(name), connection);
}

void SimpleCatalog::AddSequence(absl::string_view name,
                                const Sequence* sequence) {
  absl::MutexLock l(&mutex_);
  zetasql_base::InsertOrDie(&sequences_, absl::AsciiStrToLower(name), sequence);
}

void SimpleCatalog::AddType(absl::string_view name, const Type* type) {
  absl::MutexLock l(&mutex_);
  ABSL_CHECK(types_.insert({absl::AsciiStrToLower(name), type}).second);
}

void SimpleCatalog::AddCatalog(const std::string& name, Catalog* catalog) {
  absl::MutexLock l(&mutex_);
  AddCatalogLocked(name, catalog);
}

void SimpleCatalog::AddCatalogLocked(absl::string_view name, Catalog* catalog) {
  zetasql_base::InsertOrDie(&catalogs_, absl::AsciiStrToLower(name), catalog);
}

void SimpleCatalog::AddFunctionLocked(const std::string& name,
                                      const Function* function) {
  zetasql_base::InsertOrDie(&functions_, absl::AsciiStrToLower(name), function);
  if (!function->alias_name().empty() &&
      zetasql_base::CaseCompare(function->alias_name(), name) != 0) {
    zetasql_base::InsertOrDie(&functions_, absl::AsciiStrToLower(function->alias_name()),
                     function);
  }
}

void SimpleCatalog::AddFunction(const std::string& name,
                                const Function* function) {
  absl::MutexLock l(&mutex_);
  AddFunctionLocked(name, function);
}

void SimpleCatalog::AddTableValuedFunctionLocked(
    const std::string& name, const TableValuedFunction* table_function) {
  zetasql_base::InsertOrDie(&table_valued_functions_, absl::AsciiStrToLower(name),
                   table_function);
}

void SimpleCatalog::AddTableValuedFunction(
    const std::string& name, const TableValuedFunction* function) {
  absl::MutexLock l(&mutex_);
  AddTableValuedFunctionLocked(name, function);
}

void SimpleCatalog::AddProcedure(absl::string_view name,
                                 const Procedure* procedure) {
  absl::MutexLock l(&mutex_);
  zetasql_base::InsertOrDie(&procedures_, absl::AsciiStrToLower(name), procedure);
}

void SimpleCatalog::AddConstant(const std::string& name,
                                const Constant* constant) {
  absl::MutexLock l(&mutex_);
  AddConstantLocked(name, constant);
}

void SimpleCatalog::AddConstantLocked(const std::string& name,
                                      const Constant* constant) {
  zetasql_base::InsertOrDie(&constants_, absl::AsciiStrToLower(name), constant);
}

void SimpleCatalog::AddOwnedTable(absl::string_view name,
                                  std::unique_ptr<const Table> table) {
  AddTable(name, table.get());
  absl::MutexLock l(&mutex_);
  owned_tables_.push_back(std::move(table));
}

bool SimpleCatalog::AddOwnedTableIfNotPresent(
    absl::string_view name, std::unique_ptr<const Table> table) {
  absl::MutexLock l(&mutex_);
  const std::string canonical_name = absl::AsciiStrToLower(name);
  if (!zetasql_base::InsertIfNotPresent(&global_names_, canonical_name) ||
      !zetasql_base::InsertIfNotPresent(&tables_, canonical_name, table.get())) {
    return false;
  }
  owned_tables_.emplace_back(std::move(table));
  return true;
}

void SimpleCatalog::AddOwnedTable(absl::string_view name, const Table* table) {
  AddOwnedTable(name, absl::WrapUnique(table));
}

void SimpleCatalog::AddOwnedModel(const std::string& name,
                                  std::unique_ptr<const Model> model) {
  AddModel(name, model.get());
  absl::MutexLock l(&mutex_);
  owned_models_.emplace_back(std::move(model));
}

void SimpleCatalog::AddOwnedModel(const std::string& name, const Model* model) {
  AddOwnedModel(name, absl::WrapUnique(model));
}

void SimpleCatalog::AddOwnedCatalog(const std::string& name,
                                    std::unique_ptr<Catalog> catalog) {
  AddCatalog(name, catalog.get());
  absl::MutexLock l(&mutex_);
  owned_catalogs_.push_back(std::move(catalog));
}

void SimpleCatalog::AddOwnedCatalog(const std::string& name, Catalog* catalog) {
  AddOwnedCatalog(name, absl::WrapUnique(catalog));
}

void SimpleCatalog::AddOwnedFunction(const std::string& name,
                                     std::unique_ptr<const Function> function) {
  absl::MutexLock l(&mutex_);
  AddOwnedFunctionLocked(name, std::move(function));
}

void SimpleCatalog::AddOwnedFunction(const std::string& name,
                                     const Function* function) {
  AddOwnedFunction(name, absl::WrapUnique(function));
}

void SimpleCatalog::AddOwnedFunctionLocked(
    const std::string& name, std::unique_ptr<const Function> function) {
  AddFunctionLocked(name, function.get());
  owned_functions_.emplace_back(std::move(function));
}

void SimpleCatalog::AddOwnedTableValuedFunction(
    const std::string& name,
    std::unique_ptr<const TableValuedFunction> function) {
  AddTableValuedFunction(name, function.get());
  absl::MutexLock l(&mutex_);
  owned_table_valued_functions_.emplace_back(std::move(function));
}

void SimpleCatalog::AddOwnedTableValuedFunction(
    const std::string& name, const TableValuedFunction* function) {
  AddOwnedTableValuedFunction(name, absl::WrapUnique(function));
}

void SimpleCatalog::AddOwnedTableValuedFunctionLocked(
    const std::string& name,
    std::unique_ptr<const TableValuedFunction> table_function) {
  AddTableValuedFunctionLocked(name, table_function.get());
  owned_table_valued_functions_.emplace_back(std::move(table_function));
}

void SimpleCatalog::AddOwnedProcedure(
    const std::string& name, std::unique_ptr<const Procedure> procedure) {
  AddProcedure(name, procedure.get());
  absl::MutexLock l(&mutex_);
  owned_procedures_.push_back(std::move(procedure));
}

bool SimpleCatalog::AddOwnedProcedureIfNotPresent(
    std::unique_ptr<Procedure> procedure) {
  absl::MutexLock l(&mutex_);
  if (!zetasql_base::InsertIfNotPresent(&procedures_,
                               absl::AsciiStrToLower(procedure->Name()),
                               procedure.get())) {
    return false;
  }
  owned_procedures_.emplace_back(std::move(procedure));
  return true;
}

void SimpleCatalog::AddOwnedProcedure(const std::string& name,
                                      const Procedure* procedure) {
  AddOwnedProcedure(name, absl::WrapUnique(procedure));
}

void SimpleCatalog::AddOwnedConstant(const std::string& name,
                                     std::unique_ptr<const Constant> constant) {
  AddConstant(name, constant.get());
  absl::MutexLock l(&mutex_);
  owned_constants_.push_back(std::move(constant));
}

void SimpleCatalog::AddOwnedConstant(const std::string& name,
                                     const Constant* constant) {
  AddOwnedConstant(name, absl::WrapUnique(constant));
}

void SimpleCatalog::AddTable(const Table* table) {
  AddTable(table->Name(), table);
}

void SimpleCatalog::AddModel(const Model* model) {
  AddModel(model->Name(), model);
}

void SimpleCatalog::AddConnection(const Connection* connection) {
  AddConnection(connection->Name(), connection);
}

void SimpleCatalog::AddSequence(const Sequence* sequence) {
  AddSequence(sequence->Name(), sequence);
}

void SimpleCatalog::AddCatalog(Catalog* catalog) {
  AddCatalog(catalog->FullName(), catalog);
}

void SimpleCatalog::AddFunction(const Function* function) {
  AddFunction(function->Name(), function);
}

void SimpleCatalog::AddTableValuedFunction(
    const TableValuedFunction* function) {
  AddTableValuedFunction(function->Name(), function);
}

void SimpleCatalog::AddProcedure(const Procedure* procedure) {
  AddProcedure(procedure->Name(), procedure);
}

void SimpleCatalog::AddConstant(const Constant* constant) {
  AddConstant(constant->Name(), constant);
}

void SimpleCatalog::AddOwnedTable(std::unique_ptr<const Table> table) {
  AddTable(table.get());
  absl::MutexLock l(&mutex_);
  owned_tables_.push_back(std::move(table));
}

void SimpleCatalog::AddOwnedTable(const Table* table) {
  AddOwnedTable(absl::WrapUnique(table));
}

void SimpleCatalog::AddOwnedModel(std::unique_ptr<const Model> model) {
  AddModel(model.get());
  absl::MutexLock l(&mutex_);
  owned_models_.emplace_back(std::move(model));
}

void SimpleCatalog::AddOwnedModel(const Model* model) {
  AddOwnedModel(absl::WrapUnique(model));
}

bool SimpleCatalog::AddOwnedModelIfNotPresent(
    std::unique_ptr<const Model> model) {
  absl::MutexLock l(&mutex_);
  if (!zetasql_base::InsertIfNotPresent(&models_, absl::AsciiStrToLower(model->Name()),
                               model.get())) {
    return false;
  }
  owned_models_.push_back(std::move(model));
  return true;
}

void SimpleCatalog::AddOwnedCatalog(std::unique_ptr<Catalog> catalog) {
  absl::MutexLock l(&mutex_);
  const std::string name = catalog->FullName();
  AddOwnedCatalogLocked(name, std::move(catalog));
}

void SimpleCatalog::AddOwnedCatalog(Catalog* catalog) {
  AddOwnedCatalog(absl::WrapUnique(catalog));
}

void SimpleCatalog::AddOwnedCatalogLocked(const std::string& name,
                                          std::unique_ptr<Catalog> catalog) {
  AddCatalogLocked(name, catalog.get());
  owned_catalogs_.emplace_back(std::move(catalog));
}

bool SimpleCatalog::AddOwnedCatalogIfNotPresent(
    const std::string& name, std::unique_ptr<Catalog> catalog) {
  absl::MutexLock l(&mutex_);
  if (catalogs_.contains(absl::AsciiStrToLower(name))) {
    return false;
  }
  AddOwnedCatalogLocked(name, std::move(catalog));
  return true;
}

void SimpleCatalog::AddOwnedFunction(std::unique_ptr<const Function> function) {
  AddFunction(function->Name(), function.get());
  absl::MutexLock l(&mutex_);
  owned_functions_.push_back(std::move(function));
}

void SimpleCatalog::AddOwnedFunction(const Function* function) {
  AddOwnedFunction(function->Name(), absl::WrapUnique(function));
}

bool SimpleCatalog::AddOwnedFunctionIfNotPresent(
    const std::string& name, std::unique_ptr<Function>* function) {
  absl::MutexLock l(&mutex_);
  // If the function name exists, return false.
  if (functions_.contains(absl::AsciiStrToLower(name))) {
    return false;
  }
  const std::string alias_name = (*function)->alias_name();
  // If the function has an alias and the alias exists, return false.
  if (!alias_name.empty() &&
      zetasql_base::CaseCompare(alias_name, name) != 0) {
    if (functions_.contains(absl::AsciiStrToLower(alias_name))) {
      return false;
    }
  }
  AddOwnedFunctionLocked(name, std::move(*function));
  return true;
}

bool SimpleCatalog::AddOwnedFunctionIfNotPresent(
    std::unique_ptr<Function>* function) {
  return AddOwnedFunctionIfNotPresent((*function)->Name(), function);
}

void SimpleCatalog::AddOwnedTableValuedFunction(
    std::unique_ptr<const TableValuedFunction> function) {
  AddTableValuedFunction(function.get());
  absl::MutexLock l(&mutex_);
  owned_table_valued_functions_.push_back(std::move(function));
}

void SimpleCatalog::AddOwnedTableValuedFunction(
    const TableValuedFunction* function) {
  AddOwnedTableValuedFunction(absl::WrapUnique(function));
}

bool SimpleCatalog::AddOwnedTableValuedFunctionIfNotPresent(
    const std::string& name,
    std::unique_ptr<TableValuedFunction>* table_function) {
  absl::MutexLock l(&mutex_);
  // If the table function name exists, return false.
  if (table_valued_functions_.contains(absl::AsciiStrToLower(name))) {
    return false;
  }
  AddOwnedTableValuedFunctionLocked(name, std::move(*table_function));
  return true;
}

bool SimpleCatalog::AddOwnedTableValuedFunctionIfNotPresent(
    std::unique_ptr<TableValuedFunction>* table_function) {
  return AddOwnedTableValuedFunctionIfNotPresent((*table_function)->Name(),
                                                 table_function);
}

bool SimpleCatalog::AddTypeIfNotPresent(absl::string_view name,
                                        const Type* type) {
  absl::MutexLock l(&mutex_);
  return types_.insert({absl::AsciiStrToLower(name), type}).second;
}

void SimpleCatalog::AddOwnedProcedure(
    std::unique_ptr<const Procedure> procedure) {
  AddProcedure(procedure.get());
  absl::MutexLock l(&mutex_);
  owned_procedures_.emplace_back(std::move(procedure));
}

void SimpleCatalog::AddOwnedProcedure(const Procedure* procedure) {
  AddOwnedProcedure(absl::WrapUnique(procedure));
}

void SimpleCatalog::AddOwnedConstant(std::unique_ptr<const Constant> constant) {
  absl::MutexLock l(&mutex_);
  AddConstantLocked(constant->Name(), constant.get());
  owned_constants_.push_back(std::move(constant));
}

bool SimpleCatalog::AddOwnedConstantIfNotPresent(
    std::unique_ptr<const Constant> constant) {
  absl::MutexLock l(&mutex_);
  if (!zetasql_base::InsertIfNotPresent(&constants_,
                               absl::AsciiStrToLower(constant->Name()),
                               constant.get())) {
    return false;
  }
  owned_constants_.push_back(std::move(constant));
  return true;
}

void SimpleCatalog::AddOwnedConstant(const Constant* constant) {
  AddOwnedConstant(absl::WrapUnique(constant));
}

void SimpleCatalog::AddOwnedConnection(
    const std::string& name, std::unique_ptr<const Connection> connection) {
  AddConnection(name, connection.get());
  absl::MutexLock l(&mutex_);
  owned_connections_.push_back(std::move(connection));
}

void SimpleCatalog::AddOwnedConnection(
    std::unique_ptr<const Connection> connection) {
  AddConnection(connection.get());
  absl::MutexLock l(&mutex_);
  owned_connections_.push_back(std::move(connection));
}

bool SimpleCatalog::AddOwnedConnectionIfNotPresent(
    std::unique_ptr<const Connection> connection) {
  absl::MutexLock l(&mutex_);
  if (!zetasql_base::InsertIfNotPresent(&connections_,
                               absl::AsciiStrToLower(connection->Name()),
                               connection.get())) {
    return false;
  }
  owned_connections_.push_back(std::move(connection));
  return true;
}

SimpleCatalog* SimpleCatalog::MakeOwnedSimpleCatalog(absl::string_view name) {
  SimpleCatalog* new_catalog = new SimpleCatalog(name, type_factory());
  AddOwnedCatalog(new_catalog);
  return new_catalog;
}

void SimpleCatalog::SetDescriptorPool(const google::protobuf::DescriptorPool* pool) {
  absl::MutexLock l(&mutex_);
  ABSL_CHECK(descriptor_pool_ == nullptr)
      << "SimpleCatalog::SetDescriptorPool can only be called once";
  owned_descriptor_pool_.reset();
  descriptor_pool_ = pool;
}

void SimpleCatalog::SetOwnedDescriptorPool(
    std::unique_ptr<const google::protobuf::DescriptorPool> pool) {
  absl::MutexLock l(&mutex_);
  ABSL_CHECK(descriptor_pool_ == nullptr)
      << "SimpleCatalog::SetDescriptorPool can only be called once";
  owned_descriptor_pool_ = std::move(pool);
  descriptor_pool_ = owned_descriptor_pool_.get();
}

void SimpleCatalog::AddZetaSQLFunctions(
    const std::vector<const Function*>& functions) {
  TypeFactory* type_factory = this->type_factory();
  absl::MutexLock l(&mutex_);

  for (const auto& function : functions) {
    const std::vector<std::string>& path = function->FunctionNamePath();
    SimpleCatalog* catalog = this;
    if (path.size() > 1) {
      ABSL_CHECK_LE(path.size(), 2);
      const std::string& space = path[0];
      auto sub_entry = owned_zetasql_subcatalogs_.find(space);
      if (sub_entry != owned_zetasql_subcatalogs_.end()) {
        catalog = sub_entry->second.get();
        ABSL_CHECK(catalog != nullptr) << "internal state corrupt: " << space;
      } else {
        auto new_catalog = std::make_unique<SimpleCatalog>(space, type_factory);
        AddCatalogLocked(space, new_catalog.get());
        catalog = new_catalog.get();
        ABSL_CHECK(
            owned_zetasql_subcatalogs_.emplace(space, std::move(new_catalog))
                .second);
      }
    }
    catalog->AddFunctionLocked(path.back(), function);
  }
}

absl::Status SimpleCatalog::AddBuiltinFunctionsAndTypesImpl(
    const BuiltinFunctionOptions& options, bool add_types) {
  absl::flat_hash_map<std::string, std::unique_ptr<Function>> function_map;
  // We have to call type_factory() while not holding mutex_.
  TypeFactory* type_factory = this->type_factory();
  absl::flat_hash_map<std::string, const Type*> type_map;

  ZETASQL_RETURN_IF_ERROR(GetBuiltinFunctionsAndTypes(options, *type_factory,
                                              function_map, type_map));
  for (auto& function_pair : function_map) {
    const std::vector<std::string>& path =
        function_pair.second->FunctionNamePath();
    SimpleCatalog* catalog = this;
    if (path.size() > 1) {
      ZETASQL_RET_CHECK_LE(path.size(), 2);
      absl::MutexLock l(&mutex_);
      const std::string& space = path[0];
      auto sub_entry = owned_zetasql_subcatalogs_.find(space);
      if (sub_entry != owned_zetasql_subcatalogs_.end()) {
        catalog = sub_entry->second.get();
        ZETASQL_RET_CHECK(catalog != nullptr) << "internal state corrupt: " << space;
      } else {
        auto new_catalog = std::make_unique<SimpleCatalog>(space, type_factory);
        AddCatalogLocked(space, new_catalog.get());
        catalog = new_catalog.get();
        ZETASQL_RET_CHECK(
            owned_zetasql_subcatalogs_.emplace(space, std::move(new_catalog))
                .second);
      }
    }
    catalog->AddOwnedFunction(path.back(), std::move(function_pair.second));
  }
  if (add_types) {
    for (const auto& [name, type] : type_map) {
      AddTypeIfNotPresent(name, type);
    }
  }
  return absl::OkStatus();
}

void SimpleCatalog::AddBuiltinFunctions(const BuiltinFunctionOptions& options) {
  absl::Status status =
      this->AddBuiltinFunctionsAndTypesImpl(options, /*add_types=*/false);
  ZETASQL_DCHECK_OK(status);
}

absl::Status SimpleCatalog::AddBuiltinFunctionsAndTypes(
    const BuiltinFunctionOptions& options) {
  return this->AddBuiltinFunctionsAndTypesImpl(options, /*add_types=*/true);
}

int SimpleCatalog::RemoveTypes(std::function<bool(const Type*)> predicate) {
  absl::MutexLock l(&mutex_);
  int num_removed = 0;
  for (const auto& [_, sub_catalog] : owned_zetasql_subcatalogs_) {
    num_removed += sub_catalog->RemoveTypes(predicate);
  }
  for (auto& [_, sub_catalog] : catalogs_) {
    if (sub_catalog->Is<SimpleCatalog>()) {
      num_removed +=
          sub_catalog->GetAs<SimpleCatalog>()->RemoveTypes(predicate);
    }
  }
  num_removed += absl::erase_if(
      types_, [predicate](std::pair<const std::string, const Type*> pair) {
        return predicate(pair.second);
      });
  return num_removed;
}

int SimpleCatalog::RemoveFunctionsLocked(
    std::function<bool(const Function*)> predicate,
    std::vector<std::unique_ptr<const Function>>& removed) {
  int num_removed = 0;
  for (const auto& [_, sub_catalog] : owned_zetasql_subcatalogs_) {
    num_removed += sub_catalog->RemoveFunctions(predicate, removed);
  }
  for (auto& [_, sub_catalog] : catalogs_) {
    if (sub_catalog == this) {
      // This avoids deadlock for recursive simple catalogs.
      // TODO: Support a indirectly recursive example in
      //     SampleCatalog and improve this to detect indirect recursion.
      continue;
    }
    if (sub_catalog->Is<SimpleCatalog>()) {
      num_removed += sub_catalog->GetAs<SimpleCatalog>()->RemoveFunctions(
          predicate, removed);
    }
  }
  num_removed += absl::erase_if(
      functions_,
      [predicate](std::pair<const std::string, const Function*> pair) {
        return predicate(pair.second);
      });
  for (auto it = owned_functions_.begin(); it < owned_functions_.end(); ++it) {
    if (predicate(it->get())) {
      removed.emplace_back(std::move(*it));
      owned_functions_.erase(it);
    }
  }
  return num_removed;
}

int SimpleCatalog::RemoveFunctions(
    std::function<bool(const Function*)> predicate) {
  absl::MutexLock l(&mutex_);
  std::vector<std::unique_ptr<const Function>> removed;
  return RemoveFunctionsLocked(predicate, removed);
}

int SimpleCatalog::RemoveTableValuedFunctionsLocked(
    std::function<bool(const TableValuedFunction*)> predicate,
    std::vector<std::unique_ptr<const TableValuedFunction>>& removed) {
  int num_removed = 0;
  for (const auto& [_, sub_catalog] : owned_zetasql_subcatalogs_) {
    num_removed += sub_catalog->RemoveTableValuedFunctions(predicate, removed);
  }
  for (auto& [_, sub_catalog] : catalogs_) {
    if (sub_catalog->Is<SimpleCatalog>()) {
      num_removed +=
          sub_catalog->GetAs<SimpleCatalog>()->RemoveTableValuedFunctions(
              predicate, removed);
    }
  }
  num_removed += absl::erase_if(
      table_valued_functions_,
      [predicate](
          std::pair<const std::string, const TableValuedFunction*> pair) {
        return predicate(pair.second);
      });
  for (auto it = owned_table_valued_functions_.begin();
       it < owned_table_valued_functions_.end(); ++it) {
    if (predicate(it->get())) {
      removed.emplace_back(std::move(*it));
      owned_table_valued_functions_.erase(it);
    }
  }
  return num_removed;
}

int SimpleCatalog::RemoveTableValuedFunctions(
    std::function<bool(const TableValuedFunction*)> predicate) {
  absl::MutexLock l(&mutex_);
  std::vector<std::unique_ptr<const TableValuedFunction>> removed;
  return RemoveTableValuedFunctionsLocked(predicate, removed);
}

// DEPRECATED
void SimpleCatalog::ClearTableValuedFunctions() {
  absl::MutexLock l(&mutex_);
  table_valued_functions_.clear();
  owned_table_valued_functions_.clear();
  for (const auto& pair : owned_zetasql_subcatalogs_) {
    catalogs_.erase(pair.first);
  }
  owned_zetasql_subcatalogs_.clear();
}

TypeFactory* SimpleCatalog::type_factory() {
  absl::MutexLock l(&mutex_);
  if (type_factory_ == nullptr) {
    ABSL_DCHECK(owned_type_factory_ == nullptr);
    owned_type_factory_ = std::make_unique<TypeFactory>();
    type_factory_ = owned_type_factory_.get();
  }
  return type_factory_;
}

namespace {

absl::StatusOr<const Type*> DeserializeNamedType(
    const SimpleCatalogProto::NamedTypeProto& named_type_proto,
    const TypeDeserializer& type_deserializer) {
  if (!named_type_proto.has_type()) {
    return MakeSqlError() << "Type is missing in "
                             "zetasql::SimpleCatalogProto::NamedTypeProto: "
                          << named_type_proto.DebugString();
  }

  if (!named_type_proto.has_name()) {
    return MakeSqlError() << "Name is missing in "
                             "zetasql::SimpleCatalogProto::NamedTypeProto: "
                          << named_type_proto.DebugString();
  }

  return type_deserializer.Deserialize(named_type_proto.type());
}

template <typename M, typename ValueContainer>
void InsertValuesFromMap(const M& m, ValueContainer* value_container) {
  for (const auto& kv : m) {
    value_container->insert(kv.second);
  }
}

}  // namespace

absl::Status SimpleCatalog::DeserializeImpl(
    const SimpleCatalogProto& proto, const TypeDeserializer& type_deserializer,
    SimpleCatalog* catalog) {
  for (const SimpleTableProto& table_proto : proto.table()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<SimpleTable> table,
                     SimpleTable::Deserialize(table_proto, type_deserializer));
    const std::string& name = table_proto.has_name_in_catalog()
                                  ? table_proto.name_in_catalog()
                                  : table_proto.name();
    if (!AddOwnedTableIfNotPresent(name, std::move(table))) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate table '" << name << "' in serialized catalog";
    }
  }

  for (const SimpleCatalogProto::NamedTypeProto& named_type_proto :
       proto.named_type()) {
    ZETASQL_ASSIGN_OR_RETURN(const Type* type,
                     DeserializeNamedType(named_type_proto, type_deserializer));
    if (!AddTypeIfNotPresent(named_type_proto.name(), type)) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate type '" << named_type_proto.name()
             << "' in serialized catalog";
    }
  }

  for (const SimpleCatalogProto& catalog_proto : proto.catalog()) {
    std::unique_ptr<SimpleCatalog> sub_catalog(
        new SimpleCatalog(catalog_proto.name(), type_factory()));
    ZETASQL_RETURN_IF_ERROR(sub_catalog->DeserializeImpl(catalog_proto,
                                                 type_deserializer, catalog));
    if (!AddOwnedCatalogIfNotPresent(catalog_proto.name(),
                                     std::move(sub_catalog))) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate catalog '" << catalog_proto.name()
             << "' in serialized catalog";
    }
  }

  for (const FunctionProto& function_proto : proto.custom_function()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<Function> function,
                     Function::Deserialize(function_proto, type_deserializer));
    const std::string name = function->Name();
    if (!AddOwnedFunctionIfNotPresent(&function)) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate function '" << name << "' in serialized catalog";
    }
  }
  for (const auto& procedure_proto : proto.procedure()) {
    ZETASQL_ASSIGN_OR_RETURN(
        std::unique_ptr<Procedure> procedure,
        Procedure::Deserialize(procedure_proto, type_deserializer));
    const std::string name = procedure->Name();
    if (!AddOwnedProcedureIfNotPresent(std::move(procedure))) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate procedure '" << name << "' in serialized catalog";
    }
  }

  if (proto.has_builtin_function_options()) {
    BuiltinFunctionOptions options(proto.builtin_function_options());
    ZETASQL_RETURN_IF_ERROR(AddBuiltinFunctionsAndTypes(options));
  }

  for (const TableValuedFunctionProto& tvf_proto : proto.custom_tvf()) {
    // TableValuedFunction::Deserialize.
    std::unique_ptr<TableValuedFunction> tvf;
    ZETASQL_RETURN_IF_ERROR(
        TableValuedFunction::Deserialize(tvf_proto, type_deserializer, &tvf));
    const std::string name = tvf->Name();
    if (!AddOwnedTableValuedFunctionIfNotPresent(&tvf)) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate TVF '" << name << "' in serialized catalog";
    }
  }

  for (const auto& constant_proto : proto.constant()) {
    ZETASQL_ASSIGN_OR_RETURN(
        std::unique_ptr<SimpleConstant> constant,
        SimpleConstant::Deserialize(constant_proto, type_deserializer));
    const std::string name = constant->Name();
    if (!AddOwnedConstantIfNotPresent(std::move(constant))) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate constant '" << name << "' in serialized catalog";
    }
  }

  for (const auto& connection_proto : proto.connection()) {
    std::unique_ptr<SimpleConnection> connection =
        SimpleConnection::Deserialize(connection_proto);
    const std::string name = connection->Name();
    if (!AddOwnedConnectionIfNotPresent(std::move(connection))) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate connection '" << name << "' in serialized catalog";
    }
  }

  for (const auto& model_proto : proto.model()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<Model> model,
                     SimpleModel::Deserialize(model_proto, type_deserializer));
    const std::string name = model->Name();
    if (!AddOwnedModelIfNotPresent(std::move(model))) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate model '" << name << "' in serialized catalog";
    }
  }

  if (proto.has_file_descriptor_set_index()) {
    SetDescriptorPool(
        type_deserializer
            .descriptor_pools()[proto.file_descriptor_set_index()]);
  }

  return absl::OkStatus();
}

absl::Status SimpleCatalog::Deserialize(
    const SimpleCatalogProto& proto,
    const std::vector<const google::protobuf::DescriptorPool*>& pools,
    std::unique_ptr<SimpleCatalog>* result) {
  ZETASQL_ASSIGN_OR_RETURN(*result, Deserialize(proto, pools));
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<SimpleCatalog>> SimpleCatalog::Deserialize(
    const SimpleCatalogProto& proto,
    const absl::Span<const google::protobuf::DescriptorPool* const> pools,
    const ExtendedTypeDeserializer* extended_type_deserializer) {
  // Create a top level catalog that owns the TypeFactory.
  std::unique_ptr<SimpleCatalog> catalog(new SimpleCatalog(proto.name()));

  ZETASQL_RETURN_IF_ERROR(
      catalog->DeserializeImpl(proto,
                               TypeDeserializer(catalog->type_factory(), pools,
                                                extended_type_deserializer),
                               catalog.get()));
  return catalog;
}

absl::Status SimpleCatalog::Serialize(
    FileDescriptorSetMap* file_descriptor_set_map,
    SimpleCatalogProto* proto,
    bool ignore_builtin,
    bool ignore_recursive) const {
  absl::flat_hash_set<const Catalog*> seen;
  return SerializeImpl(
      &seen, file_descriptor_set_map, proto,
      ignore_builtin, ignore_recursive);
}

absl::Status SimpleCatalog::SerializeImpl(
    absl::flat_hash_set<const Catalog*>* seen_catalogs,
    FileDescriptorSetMap* file_descriptor_set_map, SimpleCatalogProto* proto,
    bool ignore_builtin, bool ignore_recursive) const {
  seen_catalogs->insert(this);

  absl::MutexLock l(&mutex_);

  proto->Clear();
  proto->set_name(name_);

  // Convert hash maps to std::maps so that the serialization output is
  // deterministic.
  const absl::btree_map<std::string, const Table*> tables(tables_.begin(),
                                                          tables_.end());
  const absl::flat_hash_map<std::string, const Model*> models(models_.begin(),
                                                              models_.end());
  const absl::btree_map<std::string, const Type*> types(types_.begin(),
                                                        types_.end());
  const absl::btree_map<std::string, const Function*> functions(
      functions_.begin(), functions_.end());
  const absl::btree_map<std::string, const TableValuedFunction*>
      table_valued_functions(table_valued_functions_.begin(),
                             table_valued_functions_.end());
  const absl::flat_hash_map<std::string, const Procedure*> procedures(
      procedures_.begin(), procedures_.end());
  const absl::btree_map<std::string, const Catalog*> catalogs(catalogs_.begin(),
                                                              catalogs_.end());
  const absl::btree_map<std::string, const Constant*> constants(
      constants_.begin(), constants_.end());
  const absl::flat_hash_map<std::string, const Connection*> connections(
      connections_.begin(), connections_.end());

  for (const auto& entry : tables) {
    const std::string& table_name = entry.first;
    const Table* const table = entry.second;
    if (table->Is<SimpleSQLView>()) {
      // TODO: Serialize these too.
      continue;
    }
    if (!table->Is<SimpleTable>()) {
      return ::zetasql_base::UnknownErrorBuilder()
             << "Cannot serialize non-SimpleTable " << table_name;
    }
    const SimpleTable* const simple_table = table->GetAs<SimpleTable>();
    SimpleTableProto* const table_proto = proto->add_table();
    ZETASQL_RETURN_IF_ERROR(
        simple_table->Serialize(file_descriptor_set_map, table_proto));
    if (absl::AsciiStrToLower(table_proto->name()) != table_name) {
      table_proto->set_name_in_catalog(table_name);
    }
  }

  for (const auto& entry : types) {
    const std::string& type_name = entry.first;
    const Type* const type = entry.second;
    SimpleCatalogProto::NamedTypeProto* named_type = proto->add_named_type();
    ZETASQL_RETURN_IF_ERROR(type->SerializeToProtoAndDistinctFileDescriptors(
        named_type->mutable_type(), file_descriptor_set_map));
    named_type->set_name(type_name);
  }

  for (const auto& entry : functions) {
    const Function* const function = entry.second;
    // TODO: in case we have a function with an alias we serialize it
    // twice here (first for main entry and second time for an alias). Thus
    // when we try to deserialize it we fail, because all entries are identical
    // and we still insert an alias entry using main function name as a key.
    // To fix it we should serialize only main entry.
    if (!(ignore_builtin && function->IsZetaSQLBuiltin())) {
      ZETASQL_RETURN_IF_ERROR(function->Serialize(file_descriptor_set_map,
                                          proto->add_custom_function()));
    }
  }

  for (const auto& entry : table_valued_functions) {
    const TableValuedFunction* const table_valued_function = entry.second;
    TableValuedFunctionProto tvf_proto;
    ZETASQL_RETURN_IF_ERROR(
        table_valued_function->Serialize(file_descriptor_set_map, &tvf_proto));
    if (tvf_proto.type() == FunctionEnums::INVALID) {
      // TODO: Support serialization of SQLTableValuedFunction
      continue;
    }
    *proto->add_custom_tvf() = tvf_proto;
  }

  for (const auto& entry : procedures) {
    const Procedure* const procedure = entry.second;
    ZETASQL_RETURN_IF_ERROR(
        procedure->Serialize(file_descriptor_set_map, proto->add_procedure()));
  }

  for (const auto& entry : catalogs) {
    const std::string& catalog_name = entry.first;
    const Catalog* const catalog = entry.second;
    if (seen_catalogs->contains(catalog)) {
      if (ignore_recursive) {
        continue;
      } else {
        return ::zetasql_base::UnknownErrorBuilder()
               << "Recursive catalog not serializable.";
      }
    }

    if (ignore_builtin) {
      if (owned_zetasql_subcatalogs_.contains(catalog_name)) {
        continue;
      }
    }

    if (!catalog->Is<SimpleCatalog>()) {
      return ::zetasql_base::UnknownErrorBuilder()
             << "Cannot serialize non-SimpleCatalog " << catalog_name;
    }
    const SimpleCatalog* const simple_catalog = catalog->GetAs<SimpleCatalog>();
    ZETASQL_RETURN_IF_ERROR(simple_catalog->Serialize(file_descriptor_set_map,
                                              proto->add_catalog()));
  }

  for (const auto& entry : constants) {
    const std::string& constant_name = entry.first;
    const Constant* const constant = entry.second;
    if (!constant->Is<SimpleConstant>()) {
      return ::zetasql_base::UnknownErrorBuilder()
             << "Cannot serialize non-SimpleConstant " << constant_name;
    }
    const SimpleConstant* const simple_constant =
        constant->GetAs<SimpleConstant>();
    ZETASQL_RETURN_IF_ERROR(simple_constant->Serialize(file_descriptor_set_map,
                                               proto->add_constant()));
  }

  for (const auto& entry : connections) {
    const std::string& connection_name = entry.first;
    const Connection* const connection = entry.second;
    if (!connection->Is<SimpleConnection>()) {
      return ::zetasql_base::UnknownErrorBuilder()
             << "Cannot serialize non-SimpleConnection " << connection_name;
    }
    const SimpleConnection* const simple_connection =
        connection->GetAs<SimpleConnection>();
    simple_connection->Serialize(proto->add_connection());
  }

  for (const auto& entry : models) {
    const std::string& model_name = entry.first;
    const Model* const model = entry.second;
    if (!model->Is<SimpleModel>()) {
      return ::zetasql_base::UnknownErrorBuilder()
             << "Cannot serialize non-SimpleModel " << model_name;
    }
    const SimpleModel* const simple_model = model->GetAs<SimpleModel>();
    ZETASQL_RETURN_IF_ERROR(
        simple_model->Serialize(file_descriptor_set_map, proto->add_model()));
  }

  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetCatalogs(
    absl::flat_hash_set<const Catalog*>* output) const {
  ZETASQL_RET_CHECK_NE(output, nullptr);
  ZETASQL_RET_CHECK(output->empty());
  absl::MutexLock lock(&mutex_);
  InsertValuesFromMap(catalogs_, output);
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetTables(
    absl::flat_hash_set<const Table*>* output) const {
  ZETASQL_RET_CHECK_NE(output, nullptr);
  ZETASQL_RET_CHECK(output->empty());
  absl::MutexLock lock(&mutex_);
  InsertValuesFromMap(tables_, output);
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetTypes(
    absl::flat_hash_set<const Type*>* output) const {
  ZETASQL_RET_CHECK_NE(output, nullptr);
  ZETASQL_RET_CHECK(output->empty());
  absl::MutexLock lock(&mutex_);
  InsertValuesFromMap(types_, output);
  return absl::OkStatus();
}

absl::Status SimpleCatalog::GetFunctions(
    absl::flat_hash_set<const Function*>* output) const {
  ZETASQL_RET_CHECK_NE(output, nullptr);
  ZETASQL_RET_CHECK(output->empty());
  absl::MutexLock lock(&mutex_);
  InsertValuesFromMap(functions_, output);
  return absl::OkStatus();
}

std::vector<std::string> SimpleCatalog::table_names() const {
  absl::MutexLock l(&mutex_);
  std::vector<std::string> table_names;
  zetasql_base::AppendKeysFromMap(tables_, &table_names);
  return table_names;
}

std::vector<const Table*> SimpleCatalog::tables() const {
  absl::MutexLock l(&mutex_);
  std::vector<const Table*> tables;
  zetasql_base::AppendValuesFromMap(tables_, &tables);
  return tables;
}

std::vector<const Type*> SimpleCatalog::types() const {
  absl::MutexLock l(&mutex_);
  std::vector<const Type*> types;
  zetasql_base::AppendValuesFromMap(types_, &types);
  return types;
}

std::vector<std::string> SimpleCatalog::function_names() const {
  absl::MutexLock l(&mutex_);
  std::vector<std::string> function_names;
  zetasql_base::AppendKeysFromMap(functions_, &function_names);
  return function_names;
}

std::vector<const Function*> SimpleCatalog::functions() const {
  absl::MutexLock l(&mutex_);
  std::vector<const Function*> functions;
  zetasql_base::AppendValuesFromMap(functions_, &functions);
  return functions;
}

std::vector<std::string> SimpleCatalog::table_valued_function_names() const {
  absl::MutexLock l(&mutex_);
  std::vector<std::string> table_valued_function_names;
  zetasql_base::AppendKeysFromMap(table_valued_functions_, &table_valued_function_names);
  return table_valued_function_names;
}

std::vector<const TableValuedFunction*> SimpleCatalog::table_valued_functions()
    const {
  absl::MutexLock l(&mutex_);
  std::vector<const TableValuedFunction*> table_valued_functions;
  zetasql_base::AppendValuesFromMap(table_valued_functions_, &table_valued_functions);
  return table_valued_functions;
}

std::vector<const Procedure*> SimpleCatalog::procedures() const {
  absl::MutexLock l(&mutex_);
  std::vector<const Procedure*> procedures;
  zetasql_base::AppendValuesFromMap(procedures_, &procedures);
  return procedures;
}

std::vector<std::string> SimpleCatalog::catalog_names() const {
  absl::MutexLock l(&mutex_);
  std::vector<std::string> catalog_names;
  zetasql_base::AppendKeysFromMap(catalogs_, &catalog_names);
  return catalog_names;
}

std::vector<Catalog*> SimpleCatalog::catalogs() const {
  absl::MutexLock l(&mutex_);
  std::vector<Catalog*> catalogs;
  zetasql_base::AppendValuesFromMap(catalogs_, &catalogs);
  return catalogs;
}

std::vector<std::string> SimpleCatalog::model_names() const {
  absl::MutexLock l(&mutex_);
  std::vector<std::string> model_names;
  zetasql_base::AppendKeysFromMap(models_, &model_names);
  return model_names;
}

std::vector<const Model*> SimpleCatalog::models() const {
  absl::MutexLock l(&mutex_);
  std::vector<const Model*> models;
  zetasql_base::AppendValuesFromMap(models_, &models);
  return models;
}

std::vector<std::string> SimpleCatalog::constant_names() const {
  absl::MutexLock l(&mutex_);
  std::vector<std::string> constant_names;
  zetasql_base::AppendKeysFromMap(constants_, &constant_names);
  return constant_names;
}

std::vector<const Constant*> SimpleCatalog::constants() const {
  absl::MutexLock l(&mutex_);
  std::vector<const Constant*> constants;
  zetasql_base::AppendValuesFromMap(constants_, &constants);
  return constants;
}

std::vector<std::string> SimpleCatalog::connection_names() const {
  absl::MutexLock l(&mutex_);
  std::vector<std::string> connection_names;
  zetasql_base::AppendKeysFromMap(connections_, &connection_names);
  return connection_names;
}

std::vector<const Connection*> SimpleCatalog::connections() const {
  absl::MutexLock l(&mutex_);
  std::vector<const Connection*> connections;
  zetasql_base::AppendValuesFromMap(connections_, &connections);
  return connections;
}

SimpleTable::SimpleTable(absl::string_view name,
                         const std::vector<NameAndType>& columns,
                         const int64_t serialization_id)
    : name_(name), id_(serialization_id) {
  for (const NameAndType& name_and_type : columns) {
    std::unique_ptr<SimpleColumn> column(
        new SimpleColumn(name_, name_and_type.first, name_and_type.second));
    ZETASQL_CHECK_OK(AddColumn(column.release(), true /* is_owned */));
  }
}

SimpleTable::SimpleTable(absl::string_view name,
                         const std::vector<NameAndAnnotatedType>& columns,
                         const int64_t serialization_id)
    : name_(name), id_(serialization_id) {
  for (const NameAndAnnotatedType& name_and_annotated_type : columns) {
    auto column = std::make_unique<SimpleColumn>(
        name_, name_and_annotated_type.first, name_and_annotated_type.second);
    ZETASQL_CHECK_OK(AddColumn(column.release(), /*is_owned=*/true));
  }
}

SimpleTable::SimpleTable(absl::string_view name,
                         const std::vector<const Column*>& columns,
                         bool take_ownership, const int64_t serialization_id)
    : name_(name), id_(serialization_id) {
  for (const Column* column : columns) {
    ZETASQL_CHECK_OK(AddColumn(column, take_ownership));
  }
}

// TODO: Consider changing the implicit name of the
// value table column to match the table name, rather than hardcoding
// this to "value".  Generally this should not be user-facing, but there
// are some cases where this appears in error messages and a reference
// to something named 'value' is confusing there.
SimpleTable::SimpleTable(absl::string_view name, const Type* row_type,
                         const int64_t id)
    : SimpleTable(name, {{"value", row_type}}, id) {
  is_value_table_ = true;
}

SimpleTable::SimpleTable(absl::string_view name, const int64_t id)
    : name_(name), id_(id) {}

absl::Status SimpleTable::SetAnonymizationInfo(
    const std::string& userid_column_name) {
  ZETASQL_ASSIGN_OR_RETURN(
      anonymization_info_,
      AnonymizationInfo::Create(this, absl::MakeSpan(&userid_column_name, 1)));
  return absl::OkStatus();
}

absl::Status SimpleTable::SetAnonymizationInfo(
    absl::Span<const std::string> userid_column_name_path) {
  ZETASQL_ASSIGN_OR_RETURN(anonymization_info_,
                   AnonymizationInfo::Create(this, userid_column_name_path));
  return absl::OkStatus();
}

const Column* SimpleTable::FindColumnByName(const std::string& name) const {
  if (name.empty()) {
    return nullptr;
  }
  return zetasql_base::FindPtrOrNull(columns_map_, absl::AsciiStrToLower(name));
}

absl::Status SimpleTable::AddColumn(const Column* column, bool is_owned) {
  std::unique_ptr<const Column> column_owner;
  if (is_owned) {
    column_owner.reset(column);
  }
  ZETASQL_RETURN_IF_ERROR(InsertColumnToColumnMap(column));
  columns_.push_back(column);
  if (is_owned) {
    owned_columns_.emplace_back(std::move(column_owner));
  }
  return absl::OkStatus();
}

absl::Status SimpleTable::SetPrimaryKey(std::vector<int> primary_key) {
  for (int column_index : primary_key) {
    if (column_index >= NumColumns()) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Invalid column index " << column_index << "in primary key";
    }
  }
  primary_key_.emplace(primary_key);
  return absl::OkStatus();
}

absl::Status SimpleTable::InsertColumnToColumnMap(const Column* column) {
  const std::string column_name = absl::AsciiStrToLower(column->Name());
  if (!allow_anonymous_column_name_ && column_name.empty()) {
    return ::zetasql_base::InvalidArgumentErrorBuilder()
           << "Empty column names not allowed";
  }

  if (columns_map_.contains(column_name)) {
    if (!allow_duplicate_column_names_) {
      return ::zetasql_base::InvalidArgumentErrorBuilder()
             << "Duplicate column in " << FullName() << ": " << column->Name();
    }
    columns_map_.erase(column_name);
    ZETASQL_RET_CHECK(zetasql_base::InsertIfNotPresent(&duplicate_column_names_, column_name))
        << column_name;
  } else if (!duplicate_column_names_.contains(column_name)) {
    ZETASQL_RET_CHECK(zetasql_base::InsertIfNotPresent(&columns_map_, column_name, column))
        << column_name;
  }

  if (column_name.empty()) {
    anonymous_column_seen_ = true;
  }
  return absl::OkStatus();
}

void SimpleTable::SetContents(const std::vector<std::vector<Value>>& rows) {
  column_major_contents_.clear();
  column_major_contents_.resize(NumColumns());
  for (int i = 0; i < NumColumns(); ++i) {
    auto column_values = std::make_shared<std::vector<Value>>();
    column_values->reserve(rows.size());
    for (int j = 0; j < rows.size(); ++j) {
      column_values->push_back(rows[j][i]);
    }
    column_major_contents_[i] = column_values;
  }

  num_rows_ = rows.size();
  auto factory = [this](absl::Span<const int> column_idxs)
      -> absl::StatusOr<std::unique_ptr<EvaluatorTableIterator>> {
    std::vector<const Column*> columns;
    std::vector<std::shared_ptr<const std::vector<Value>>> column_values;
    column_values.reserve(column_idxs.size());
    for (const int column_idx : column_idxs) {
      columns.push_back(GetColumn(column_idx));
      column_values.push_back(column_major_contents_[column_idx]);
    }
    std::unique_ptr<EvaluatorTableIterator> iter(
        new SimpleEvaluatorTableIterator(
            columns, column_values, num_rows_,
            /*end_status=*/absl::OkStatus(), /*filter_column_idxs=*/
            absl::flat_hash_set<int>(column_idxs.begin(), column_idxs.end()),
            /*cancel_cb=*/[]() {},
            /*set_deadline_cb=*/[](absl::Time t) {}, zetasql_base::Clock::RealClock()));
    return iter;
  };

  SetEvaluatorTableIteratorFactory(factory);
}

absl::StatusOr<std::unique_ptr<EvaluatorTableIterator>>
SimpleTable::CreateEvaluatorTableIterator(
    absl::Span<const int> column_idxs) const {
  if (evaluator_table_iterator_factory_ == nullptr) {
    // Returns an error.
    return Table::CreateEvaluatorTableIterator(column_idxs);
  }
  return (*evaluator_table_iterator_factory_)(column_idxs);
}

absl::Status SimpleTable::Serialize(
    FileDescriptorSetMap* file_descriptor_set_map,
    SimpleTableProto* proto) const {
  proto->Clear();
  proto->set_name(Name());
  if (GetSerializationId() > 0) {
    proto->set_serialization_id(GetSerializationId());
  }
  proto->set_is_value_table(IsValueTable());
  for (const Column* column : columns_) {
    auto* column_proto = proto->add_column();
    ZETASQL_RETURN_IF_ERROR(static_cast<const SimpleColumn*>(column)->Serialize(
        file_descriptor_set_map, column_proto));
  }
  const std::optional<const AnonymizationInfo> anonymization_info =
      GetAnonymizationInfo();
  if (anonymization_info.has_value()) {
    for (const auto& column_name_field :
         anonymization_info->UserIdColumnNamePath()) {
      proto->mutable_anonymization_info()->add_userid_column_name(
          column_name_field);
    }
  }
  if (primary_key_.has_value()) {
    for (int column_index : primary_key_.value()) {
      proto->add_primary_key_column_index(column_index);
    }
  }
  if (allow_anonymous_column_name_) {
    proto->set_allow_anonymous_column_name(true);
  }
  if (allow_duplicate_column_names_) {
    proto->set_allow_duplicate_column_names(true);
  }
  if (!full_name_.empty()) {
    proto->set_full_name(full_name_);
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<SimpleTable>> SimpleTable::Deserialize(
    const SimpleTableProto& proto, const TypeDeserializer& type_deserializer) {
  std::unique_ptr<SimpleTable> table(
      new SimpleTable(proto.name(), proto.serialization_id()));
  if (!proto.full_name().empty() && proto.full_name() != table->Name()) {
    ZETASQL_RETURN_IF_ERROR(table->set_full_name(proto.full_name()));
  }
  table->set_is_value_table(proto.is_value_table());
  ZETASQL_RETURN_IF_ERROR(table->set_allow_anonymous_column_name(
      proto.allow_anonymous_column_name()));
  ZETASQL_RETURN_IF_ERROR(table->set_allow_duplicate_column_names(
      proto.allow_duplicate_column_names()));

  for (const SimpleColumnProto& column_proto : proto.column()) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<SimpleColumn> column,
                     SimpleColumn::Deserialize(column_proto, table->FullName(),
                                               type_deserializer));
    ZETASQL_RETURN_IF_ERROR(table->AddColumn(column.release(), /*is_owned=*/true));
  }

  if (proto.primary_key_column_index_size() > 0) {
    std::vector<int> primary_key;
    for (int column_index : proto.primary_key_column_index()) {
      primary_key.push_back(column_index);
    }
    ZETASQL_RETURN_IF_ERROR(table->SetPrimaryKey(primary_key));
  }

  if (proto.has_anonymization_info()) {
    ZETASQL_RET_CHECK(!proto.anonymization_info().userid_column_name().empty());
    const std::vector<std::string> userid_column_name_path = {
        proto.anonymization_info().userid_column_name().begin(),
        proto.anonymization_info().userid_column_name().end()};
    ZETASQL_RETURN_IF_ERROR(table->SetAnonymizationInfo(userid_column_name_path));
  }

  return table;
}

SimpleColumn::SimpleColumn(absl::string_view table_name, absl::string_view name,
                           const Type* type,
                           const SimpleColumn::Attributes& attributes)
    : SimpleColumn(table_name, name,
                   AnnotatedType(type, /*annotation_map=*/nullptr),
                   attributes) {}

SimpleColumn::SimpleColumn(absl::string_view table_name, absl::string_view name,
                           AnnotatedType annotated_type,
                           const SimpleColumn::Attributes& attributes)
    : name_(name),
      full_name_(absl::StrCat(table_name, ".", name)),
      annotated_type_(annotated_type),
      attributes_(attributes) {}

SimpleColumn::~SimpleColumn() {
}

absl::Status SimpleColumn::Serialize(
    FileDescriptorSetMap* file_descriptor_set_map,
    SimpleColumnProto* proto) const {
  proto->Clear();
  proto->set_name(Name());
  ZETASQL_RETURN_IF_ERROR(GetType()->SerializeToProtoAndDistinctFileDescriptors(
      proto->mutable_type(), file_descriptor_set_map));
  if (GetTypeAnnotationMap() != nullptr) {
    ZETASQL_RETURN_IF_ERROR(
        GetTypeAnnotationMap()->Serialize(proto->mutable_annotation_map()));
  }
  proto->set_is_pseudo_column(IsPseudoColumn());
  if (!IsWritableColumn()) {
    proto->set_is_writable_column(false);
  }
  if (CanUpdateUnwritableToDefault()) {
    proto->set_can_update_unwritable_to_default(true);
  }

  // TODO: To be deprecated in later versions.
  proto->set_has_default_value(HasDefaultExpression());

  if (HasDefaultExpression() || HasGeneratedExpression()) {
    // The ResolvedExpr form of the expression is not serialized.
    proto->mutable_column_expression()->set_expression_string(
        GetExpression()->GetExpressionString());
    proto->mutable_column_expression()->set_expression_kind(
        (GetExpression()->GetExpressionKind() ==
         Column::ExpressionAttributes::ExpressionKind::DEFAULT)
            ? ExpressionAttributeProto::DEFAULT
            : ExpressionAttributeProto::GENERATED);
  }
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<SimpleColumn>> SimpleColumn::Deserialize(
    const SimpleColumnProto& proto, absl::string_view table_name,
    const TypeDeserializer& type_deserializer) {
  ZETASQL_ASSIGN_OR_RETURN(const Type* type,
                   type_deserializer.Deserialize(proto.type()));
  const AnnotationMap* annotation_map = nullptr;
  if (proto.has_annotation_map()) {
    ZETASQL_RETURN_IF_ERROR(type_deserializer.type_factory()->DeserializeAnnotationMap(
        proto.annotation_map(), &annotation_map));
  }

  SimpleColumn::Attributes attributes{
      .is_pseudo_column = proto.is_pseudo_column(),
      .is_writable_column = proto.is_writable_column(),
      .can_update_unwritable_to_default =
          proto.can_update_unwritable_to_default()};

  if (proto.has_column_expression()) {
    ExpressionAttributes::ExpressionKind expression_kind =
        (proto.column_expression().expression_kind() ==
         ExpressionAttributeProto::DEFAULT)
            ? ExpressionAttributes::ExpressionKind::DEFAULT
            : ExpressionAttributes::ExpressionKind::GENERATED;
    attributes.column_expression = SimpleColumn::ExpressionAttributes(
        expression_kind, proto.column_expression().expression_string(),
        nullptr);
  }

  return std::make_unique<SimpleColumn>(table_name, proto.name(),
                                        AnnotatedType(type, annotation_map),
                                        attributes);
}

// static
absl::StatusOr<std::unique_ptr<SimpleSQLView>> SimpleSQLView::Create(
    absl::string_view name, std::vector<NameAndType> columns,
    SqlSecurity security, bool is_value_table, const ResolvedScan* query) {
  auto view = absl::WrapUnique(
      new SimpleSQLView(name, security, is_value_table, query));
  for (int i = 0; i < columns.size(); ++i) {
    view->AddColumn(std::make_unique<SimpleColumn>(
        std::string(name), columns[i].name, columns[i].type));
  }
  return view;
}

// static
absl::Status SimpleConstant::Create(
    const std::vector<std::string>& name_path, const Value& value,
    std::unique_ptr<SimpleConstant>* simple_constant) {
  ZETASQL_RET_CHECK(!name_path.empty());
  ZETASQL_RET_CHECK(value.is_valid());
  simple_constant->reset(new SimpleConstant(name_path, value));
  return absl::OkStatus();
}

std::string SimpleConstant::DebugString() const {
  return absl::StrCat(FullName(), "=", ConstantValueDebugString());
}

std::string SimpleConstant::VerboseDebugString() const {
  return absl::StrCat(DebugString(), " (", type()->DebugString(), ")");
}

std::string SimpleConstant::ConstantValueDebugString() const {
  return value().DebugString();
}

absl::Status SimpleConstant::Serialize(
    FileDescriptorSetMap* file_descriptor_set_map,
    SimpleConstantProto* simple_constant_proto) const {
  for (const std::string& name : name_path()) {
    simple_constant_proto->add_name_path(name);
  }
  ZETASQL_RETURN_IF_ERROR(value().type()->SerializeToProtoAndDistinctFileDescriptors(
      simple_constant_proto->mutable_type(), file_descriptor_set_map));
  ZETASQL_RETURN_IF_ERROR(value().Serialize(simple_constant_proto->mutable_value()));
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<SimpleConstant>> SimpleConstant::Deserialize(
    const SimpleConstantProto& simple_constant_proto,
    const TypeDeserializer& type_deserializer) {
  std::vector<std::string> name_path;
  for (const std::string& name : simple_constant_proto.name_path()) {
    name_path.push_back(name);
  }
  ZETASQL_ASSIGN_OR_RETURN(const Type* type,
                   type_deserializer.Deserialize(simple_constant_proto.type()));
  ZETASQL_ASSIGN_OR_RETURN(Value value,
                   Value::Deserialize(simple_constant_proto.value(), type));
  return absl::WrapUnique(
      new SimpleConstant(std::move(name_path), std::move(value)));
}

SimpleModel::SimpleModel(const std::string& name,
                         const std::vector<NameAndType>& inputs,
                         const std::vector<NameAndType>& outputs,
                         const int64_t id)
    : name_(name), id_(id) {
  for (const NameAndType& name_and_type : inputs) {
    std::unique_ptr<SimpleColumn> column(
        new SimpleColumn(name, name_and_type.first, name_and_type.second));
    ZETASQL_CHECK_OK(AddInput(column.release(), true /* is_owned */));
  }
  for (const NameAndType& name_and_type : outputs) {
    std::unique_ptr<SimpleColumn> column(
        new SimpleColumn(name, name_and_type.first, name_and_type.second));
    ZETASQL_CHECK_OK(AddOutput(column.release(), true /* is_owned */));
  }
}

SimpleModel::SimpleModel(const std::string& name,
                         const std::vector<const Column*>& inputs,
                         const std::vector<const Column*>& outputs,
                         bool take_ownership, const int64_t id)
    : name_(name), id_(id) {
  for (const Column* column : inputs) {
    ZETASQL_CHECK_OK(AddInput(column, take_ownership));
  }
  for (const Column* column : outputs) {
    ZETASQL_CHECK_OK(AddOutput(column, take_ownership));
  }
}

const Column* SimpleModel::FindInputByName(const std::string& name) const {
  if (name.empty()) {
    return nullptr;
  }
  return zetasql_base::FindPtrOrNull(inputs_map_, absl::AsciiStrToLower(name));
}

const Column* SimpleModel::FindOutputByName(const std::string& name) const {
  if (name.empty()) {
    return nullptr;
  }
  return zetasql_base::FindPtrOrNull(outputs_map_, absl::AsciiStrToLower(name));
}
absl::Status SimpleModel::AddInput(const Column* column, bool is_owned) {
  std::unique_ptr<const Column> column_owner;
  if (is_owned) {
    column_owner.reset(column);
  }
  const std::string column_name = absl::AsciiStrToLower(column->Name());
  if (!zetasql_base::InsertIfNotPresent(&inputs_map_, column_name, column)) {
    return ::zetasql_base::InvalidArgumentErrorBuilder()
           << "Duplicate input column in " << FullName() << ": "
           << column->Name();
  }
  inputs_.push_back(column);
  if (is_owned) {
    owned_inputs_outputs_.emplace_back(std::move(column_owner));
  }
  return absl::OkStatus();
}

absl::Status SimpleModel::AddOutput(const Column* column, bool is_owned) {
  std::unique_ptr<const Column> column_owner;
  if (is_owned) {
    column_owner.reset(column);
  }
  const std::string column_name = absl::AsciiStrToLower(column->Name());
  if (!zetasql_base::InsertIfNotPresent(&outputs_map_, column_name, column)) {
    return ::zetasql_base::InvalidArgumentErrorBuilder()
           << "Duplicate output column in " << FullName() << ": "
           << column->Name();
  }
  outputs_.push_back(column);
  if (is_owned) {
    owned_inputs_outputs_.emplace_back(std::move(column_owner));
  }
  return absl::OkStatus();
}

absl::Status SimpleModel::Serialize(
    FileDescriptorSetMap* file_descriptor_set_map,
    SimpleModelProto* proto) const {
  proto->Clear();
  proto->set_id(id_);
  proto->set_name(name_);

  for (const Column* input : inputs_) {
    SimpleColumnProto* input_proto = proto->add_input();
    input_proto->set_name(input->Name());
    ZETASQL_RETURN_IF_ERROR(
        input->GetType()->SerializeToProtoAndDistinctFileDescriptors(
            input_proto->mutable_type(), file_descriptor_set_map));
  }

  for (const Column* output : outputs_) {
    SimpleColumnProto* output_proto = proto->add_output();
    output_proto->set_name(output->Name());
    ZETASQL_RETURN_IF_ERROR(
        output->GetType()->SerializeToProtoAndDistinctFileDescriptors(
            output_proto->mutable_type(), file_descriptor_set_map));
  }

  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<SimpleModel>> SimpleModel::Deserialize(
    const SimpleModelProto& proto, const TypeDeserializer& type_deserializer) {
  std::vector<NameAndType> inputs;
  for (const SimpleColumnProto& input_proto : proto.input()) {
    ZETASQL_ASSIGN_OR_RETURN(const Type* type,
                     type_deserializer.Deserialize(input_proto.type()));
    inputs.push_back(std::make_pair(input_proto.name(), type));
  }

  std::vector<NameAndType> outputs;
  for (const SimpleColumnProto& output_proto : proto.output()) {
    ZETASQL_ASSIGN_OR_RETURN(const Type* type,
                     type_deserializer.Deserialize(output_proto.type()));
    outputs.push_back(std::make_pair(output_proto.name(), type));
  }

  return std::make_unique<SimpleModel>(proto.name(), inputs, outputs,
                                       proto.id());
}

void SimpleConnection::Serialize(SimpleConnectionProto* proto) const {
  proto->Clear();
  proto->set_name(name_);
}

std::unique_ptr<SimpleConnection> SimpleConnection::Deserialize(
    const SimpleConnectionProto& proto) {
  return std::make_unique<SimpleConnection>(proto.name());
}

}  // namespace zetasql
