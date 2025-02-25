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

#include "zetasql/reference_impl/functions/string_with_collation.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "zetasql/public/collator.h"
#include "zetasql/public/functions/string_with_collation.h"
#include "zetasql/public/value.h"
#include "zetasql/reference_impl/function.h"
#include "absl/status/statusor.h"

namespace zetasql {
namespace {

// Function used to switch on a (function kind, output type, args.size())
// triple.
static constexpr uint64_t FCT_TYPE_ARITY(FunctionKind function_kind,
                                         TypeKind type_kind, size_t arity) {
  return (static_cast<uint64_t>(function_kind) << 32) + (type_kind << 16) +
         arity;
};

class StringWithCollationFunction : public BuiltinScalarFunction {
 public:
  using BuiltinScalarFunction::BuiltinScalarFunction;
  bool Eval(absl::Span<const TupleData* const> params,
            absl::Span<const Value> args, EvaluationContext* context,
            Value* result, absl::Status* status) const override;
};

class SplitWithCollationFunction : public SimpleBuiltinScalarFunction {
 public:
  using SimpleBuiltinScalarFunction::SimpleBuiltinScalarFunction;
  absl::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                             absl::Span<const Value> args,
                             EvaluationContext* context) const override;
};

class CollationKeyFunction : public SimpleBuiltinScalarFunction {
 public:
  using SimpleBuiltinScalarFunction::SimpleBuiltinScalarFunction;
  absl::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                             absl::Span<const Value> args,
                             EvaluationContext* context) const override;
};

class LikeWithCollationFunction : public SimpleBuiltinScalarFunction {
 public:
  using SimpleBuiltinScalarFunction::SimpleBuiltinScalarFunction;
  absl::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                             absl::Span<const Value> args,
                             EvaluationContext* context) const override;
};

class LikeAllWithCollationFunction : public SimpleBuiltinScalarFunction {
 public:
  using SimpleBuiltinScalarFunction::SimpleBuiltinScalarFunction;
  absl::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                             absl::Span<const Value> args,
                             EvaluationContext* context) const override;
};

class LikeAnyWithCollationFunction : public SimpleBuiltinScalarFunction {
 public:
  using SimpleBuiltinScalarFunction::SimpleBuiltinScalarFunction;
  absl::StatusOr<Value> Eval(absl::Span<const TupleData* const> params,
                             absl::Span<const Value> args,
                             EvaluationContext* context) const override;
};

template <typename OutType, typename FunctionType, class... Args>
bool InvokeWithCollation(FunctionType function, Value* result,
                         absl::Status* status, absl::string_view collation_name,
                         Args... args) {
  absl::StatusOr<std::unique_ptr<const ZetaSqlCollator>> collator_or_status =
      MakeSqlCollator(collation_name);
  if (!collator_or_status.ok()) {
    *status = collator_or_status.status();
    return false;
  }
  OutType out;
  if (!function(*(collator_or_status.value()), args..., &out, status)) {
    return false;
  }
  *result = Value::Make<OutType>(out);
  return true;
}

template <typename FunctionType, class... Args>
bool InvokeStringWithCollation(FunctionType function, Value* result,
                               absl::Status* status,
                               absl::string_view collation_name, Args... args) {
  absl::StatusOr<std::unique_ptr<const ZetaSqlCollator>> collator_or_status =
      MakeSqlCollator(collation_name);
  if (!collator_or_status.ok()) {
    *status = collator_or_status.status();
    return false;
  }
  std::string out;
  if (!function(*(collator_or_status.value()), args..., &out, status)) {
    return false;
  }
  *result = Value::String(out);
  return true;
}

bool StringWithCollationFunction::Eval(
    absl::Span<const TupleData* const> params, absl::Span<const Value> args,
    EvaluationContext* context, Value* result, absl::Status* status) const {
  if (HasNulls(args)) {
    *result = Value::Null(output_type());
    return true;
  }
  switch (FCT_TYPE_ARITY(kind(), args[0].type_kind(), args.size())) {
    case FCT_TYPE_ARITY(FunctionKind::kStrposWithCollation, TYPE_STRING, 3):
      return InvokeWithCollation<int64_t>(
          &functions::StrPosOccurrenceUtf8WithCollation, result, status,
          args[0].string_value(), args[1].string_value(),
          args[2].string_value(),
          /*pos=*/1, /*occurrence=*/1);
    case FCT_TYPE_ARITY(FunctionKind::kInstrWithCollation, TYPE_STRING, 3):
      return InvokeWithCollation<int64_t>(
          &functions::StrPosOccurrenceUtf8WithCollation, result, status,
          args[0].string_value(), args[1].string_value(),
          args[2].string_value(),
          /*pos=*/1, /*occurrence=*/1);
    case FCT_TYPE_ARITY(FunctionKind::kInstrWithCollation, TYPE_STRING, 4):
      return InvokeWithCollation<int64_t>(
          &functions::StrPosOccurrenceUtf8WithCollation, result, status,
          args[0].string_value(), args[1].string_value(),
          args[2].string_value(),
          /*pos=*/args[3].int64_value(), /*occurrence=*/1);
    case FCT_TYPE_ARITY(FunctionKind::kInstrWithCollation, TYPE_STRING, 5):
      return InvokeWithCollation<int64_t>(
          &functions::StrPosOccurrenceUtf8WithCollation, result, status,
          args[0].string_value(), args[1].string_value(),
          args[2].string_value(),
          /*pos=*/args[3].int64_value(), /*occurrence=*/args[4].int64_value());
    case FCT_TYPE_ARITY(FunctionKind::kStartsWithWithCollation, TYPE_STRING, 3):
      return InvokeWithCollation<bool>(&functions::StartsWithUtf8WithCollation,
                                       result, status, args[0].string_value(),
                                       args[1].string_value(),
                                       args[2].string_value());
    case FCT_TYPE_ARITY(FunctionKind::kEndsWithWithCollation, TYPE_STRING, 3):
      return InvokeWithCollation<bool>(&functions::EndsWithUtf8WithCollation,
                                       result, status, args[0].string_value(),
                                       args[1].string_value(),
                                       args[2].string_value());
    case FCT_TYPE_ARITY(FunctionKind::kReplaceWithCollation, TYPE_STRING, 4):
      return InvokeStringWithCollation(
          &functions::ReplaceUtf8WithCollation, result, status,
          args[0].string_value(), args[1].string_value(),
          args[2].string_value(), args[3].string_value());
  }
  *status = ::zetasql_base::UnimplementedErrorBuilder()
            << "Unsupported string function: " << debug_name();
  return false;
}

absl::StatusOr<Value> SplitWithCollationFunction::Eval(
    absl::Span<const TupleData* const> params, absl::Span<const Value> args,
    EvaluationContext* context) const {
  ZETASQL_RET_CHECK_GE(args.size(), 2);
  ZETASQL_RET_CHECK_LE(args.size(), 3);
  if (HasNulls(args)) return Value::Null(output_type());
  absl::Status status;
  std::vector<absl::string_view> parts;
  std::vector<Value> values;

  absl::StatusOr<std::unique_ptr<const ZetaSqlCollator>> collator_or_status =
      MakeSqlCollator(args[0].string_value());
  if (!collator_or_status.ok()) {
    return collator_or_status.status();
  }

  const std::string& delimiter =
      (args.size() == 2) ? "," : args[2].string_value();
  if (!functions::SplitUtf8WithCollation(*(collator_or_status.value()),
                                         args[1].string_value(), delimiter,
                                         &parts, &status)) {
    return status;
  }

  values.reserve(parts.size());

  for (absl::string_view s : parts) {
    values.push_back(Value::String(s));
  }
  return Value::Array(types::StringArrayType(), values);
}

absl::StatusOr<Value> CollationKeyFunction::Eval(
    absl::Span<const TupleData* const> params, absl::Span<const Value> args,
    EvaluationContext* context) const {
  ZETASQL_RET_CHECK_EQ(args.size(), 2);
  if (HasNulls(args)) {
    return Value::NullBytes();
  }

  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ZetaSqlCollator> collator,
                   MakeSqlCollator(args[1].string_value()));
  absl::Cord cord;
  ZETASQL_RETURN_IF_ERROR(collator->GetSortKeyUtf8(args[0].string_value(), &cord));
  return Value::Bytes(cord.Flatten());
}

absl::StatusOr<Value> LikeWithCollationFunction::Eval(
    absl::Span<const TupleData* const> params, absl::Span<const Value> args,
    EvaluationContext* context) const {
  ZETASQL_RET_CHECK_EQ(args.size(), 3);
  if (HasNulls(args)) {
    return Value::NullBool();
  }

  absl::StatusOr<std::unique_ptr<const ZetaSqlCollator>> collator_or_status =
      MakeSqlCollator(args[0].string_value());
  if (!collator_or_status.ok()) {
    return collator_or_status.status();
  }
  ZETASQL_ASSIGN_OR_RETURN(bool result,
                   functions::LikeUtf8WithCollation(
                       args[1].string_value(), args[2].string_value(),
                       *(collator_or_status.value())));
  return Value::Bool(result);
}

absl::StatusOr<Value> LikeAllWithCollationFunction::Eval(
    absl::Span<const TupleData* const> params, absl::Span<const Value> args,
    EvaluationContext* context) const {
  ZETASQL_RET_CHECK_GE(args.size(), 3);
  if (args[1].is_null()) {
    return Value::NullBool();
  }

  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ZetaSqlCollator> collator,
                   MakeSqlCollator(args[0].string_value()));

  bool is_rhs_element_null = false;
  auto& lhs = args[1].string_value();
  for (int rhs_idx = 2; rhs_idx < args.size(); rhs_idx++) {
    if (args[rhs_idx].is_null()) {
      is_rhs_element_null = true;
      continue;
    }
    ZETASQL_ASSIGN_OR_RETURN(bool result,
                     functions::LikeUtf8WithCollation(
                         lhs, args[rhs_idx].string_value(), *collator));
    if (!result) {
      return Value::Bool(false);
    }
  }

  return is_rhs_element_null ? Value::NullBool() : Value::Bool(true);
}

absl::StatusOr<Value> LikeAnyWithCollationFunction::Eval(
    absl::Span<const TupleData* const> params, absl::Span<const Value> args,
    EvaluationContext* context) const {
  ZETASQL_RET_CHECK_GE(args.size(), 3);
  if (args[1].is_null()) {
    return Value::NullBool();
  }

  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<const ZetaSqlCollator> collator,
                   MakeSqlCollator(args[0].string_value()));

  bool is_rhs_element_null = false;
  auto& lhs = args[1].string_value();
  for (int rhs_idx = 2; rhs_idx < args.size(); rhs_idx++) {
    if (args[rhs_idx].is_null()) {
      is_rhs_element_null = true;
      continue;
    }
    ZETASQL_ASSIGN_OR_RETURN(bool result,
                     functions::LikeUtf8WithCollation(
                         lhs, args[rhs_idx].string_value(), *collator));
    if (result) {
      return Value::Bool(true);
    }
  }

  return is_rhs_element_null ? Value::NullBool() : Value::Bool(false);
}

}  // namespace

void RegisterBuiltinStringWithCollationFunctions() {
  BuiltinFunctionRegistry::RegisterScalarFunction(
      {FunctionKind::kEndsWithWithCollation,
       FunctionKind::kReplaceWithCollation, FunctionKind::kSplitWithCollation,
       FunctionKind::kStartsWithWithCollation,
       FunctionKind::kStrposWithCollation, FunctionKind::kInstrWithCollation},
      [](FunctionKind kind, const Type* output_type) {
        return new StringWithCollationFunction(kind, output_type);
      });

  BuiltinFunctionRegistry::RegisterScalarFunction(
      {FunctionKind::kSplitWithCollation},
      [](FunctionKind kind, const Type* output_type) {
        return new SplitWithCollationFunction(kind, output_type);
      });
  BuiltinFunctionRegistry::RegisterScalarFunction(
      {FunctionKind::kCollationKey},
      [](FunctionKind kind, const Type* output_type) {
        return new CollationKeyFunction(kind, output_type);
      });
  BuiltinFunctionRegistry::RegisterScalarFunction(
      {FunctionKind::kLikeWithCollation},
      [](FunctionKind kind, const Type* output_type) {
        return new LikeWithCollationFunction(kind, output_type);
      });
  BuiltinFunctionRegistry::RegisterScalarFunction(
      {FunctionKind::kLikeAllWithCollation},
      [](FunctionKind kind, const Type* output_type) {
        return new LikeAllWithCollationFunction(kind, output_type);
      });
  BuiltinFunctionRegistry::RegisterScalarFunction(
      {FunctionKind::kLikeAnyWithCollation},
      [](FunctionKind kind, const Type* output_type) {
        return new LikeAnyWithCollationFunction(kind, output_type);
      });
}

}  // namespace zetasql
