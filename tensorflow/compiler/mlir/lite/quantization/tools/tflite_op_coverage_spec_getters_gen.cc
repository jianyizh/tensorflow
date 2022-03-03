/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <list>
#include <regex>  // NOLINT
#include <string>

#include "absl/strings/match.h"
#include "absl/strings/str_replace.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/TableGen/Main.h"
#include "llvm/TableGen/Record.h"
#include "mlir/TableGen/Operator.h"  // from @llvm-project

using llvm::LessRecord;
using llvm::raw_ostream;
using llvm::Record;
using llvm::RecordKeeper;
using mlir::tblgen::Operator;

const std::map<std::string, std::string> &GetTypeToStringRepresentation() {
  static auto *entries = new std::map<std::string, std::string>({
      {"F32", "32-bit float"},
      {"I32", "32-bit signless integer"},
      {"I64", "64-bit signless integer"},
      {"QI16", "QI16 type"},
      {"I8", "8-bit signless integer"},
      {"UI8", "8-bit unsigned integer"},
      {"QI8", "QI8 type"},
      {"QUI8", "QUI8 type"},
      {"TFL_Quint8", "TFLite quint8 type"},
  });

  return *entries;
}

void EmitDynamicRangeOp(std::vector<Record *> &defs, raw_ostream *ostream) {
  std::string dynamic_quant_kernel_support_regex =
      "bool GetDynamicRangeQuantKernelSupport() { return true; }";
  raw_ostream &os = *ostream;
  std::vector<std::string> weight_only;
  llvm::sort(defs, LessRecord());

  os.indent(0) << "const std::set<std::string> &ExportDynamicRangeSpec() {\n";
  os.indent(2) << "static const std::set<std::string> * result =\n";
  os.indent(4) << "new std::set<std::string>({\n";

  // Retrieve all the ops that have DynamicRangeQuantizedOpInterface trait.
  for (const auto *def : defs) {
    Operator op(def);
    if (!op.getTrait("DynamicRangeQuantizedOpInterface::Trait")) continue;

    auto op_name = op.getCppClassName();
    auto op_extra_declaration = op.getExtraClassDeclaration().str();

    bool kernel_support = absl::StrContains(
        absl::StrReplaceAll(op_extra_declaration, {{"\n", " "}}),
        dynamic_quant_kernel_support_regex);

    // Classify dynamic range and weight-only fallback
    if (kernel_support) {
      os.indent(6) << "\"" << op_name << "\",\n";
    } else {
      weight_only.push_back(op_name.str());
    }
  }

  os.indent(4) << "});";
  os.indent(2) << "return *result;\n";
  os.indent(0) << "}\n";

  os.indent(0)
      << "const std::set<std::string> &ExportDynamicRangeWeightOnlySpec() {\n";
  os.indent(2) << "static const std::set<std::string> * result =\n";
  os.indent(4) << "new std::set<std::string>({\n";

  // Retrieve weight-only fallback.
  for (const auto &op_name : weight_only) {
    os.indent(6) << "\"" << op_name << "\",\n";
  }

  os.indent(4) << "});";
  os.indent(2) << "return *result;\n";
  os.indent(0) << "}\n";
}

void EmitSparseOp(std::vector<Record *> &defs, raw_ostream *ostream) {
  raw_ostream &os = *ostream;
  llvm::sort(defs, LessRecord());

  os.indent(0) << "const std::set<std::string> &ExportSparsitySpec() {\n";
  os.indent(2) << "static const std::set<std::string> * result =\n";
  os.indent(4) << "new std::set<std::string>({\n";

  // Retrieve all the ops that have SparseOp trait.
  for (const auto *def : defs) {
    Operator op(def);
    if (!op.getTrait("SparseOpInterface::Trait")) {
      continue;
    }
    os.indent(6) << "\"" << op.getCppClassName() << "\",\n";
  }

  os.indent(4) << "});";
  os.indent(2) << "return *result;\n";
  os.indent(0) << "}\n";
}

bool CheckTypeConstraints(llvm::Init *input_value,
                          std::list<std::string> required_types,
                          bool per_axis) {
  auto *def_init = llvm::cast<llvm::DefInit>(input_value);
  auto *val = def_init->getDef()->getValue("tflRuntimeTypePredicate");

  // For non-per-axis op, no predicate means accepting AnyTensor.
  if (!val) return !per_axis;

  llvm::StringRef supported_types =
      def_init->getDef()->getValueAsString("tflRuntimeTypeDescription");

  for (const std::string &type : required_types) {
    if (!absl::StrContains(supported_types, type)) return false;
  }
  return true;
}

void GenerateStaticQuantOp(std::vector<Record *> &defs,
                           std::vector<std::string> &result, bool is_signed,
                           bool per_axis) {
  std::list<std::string> required_types = {
      GetTypeToStringRepresentation().at("F32")};
  if (is_signed) {
    required_types.push_back(GetTypeToStringRepresentation().at("QI8"));
  } else {
    required_types.push_back(GetTypeToStringRepresentation().at("QUI8"));
  }

  // Dimension equals to -1 means per-channel quantization is not supported for
  // the op. Therefore check whether the return value is positive integer as
  // well.
  std::regex per_channel_support_regex(
      "(.*)(int GetQuantizationDimIndex\\(\\) \\{ return (\\d*); \\})(.*)");

  for (const auto *def : defs) {
    Operator op(def);
    if (!op.getTrait("::mlir::OpTrait::quant::QuantizableResult")) continue;

    llvm::DagInit *args_in_dag = def->getValueAsDag("arguments");
    // Assumes argument name is "input" for input activations. Otherwise, assume
    // the first argument is the input activation.
    int input_idx = 0;
    for (int i = 0; i < args_in_dag->getNumArgs(); i++) {
      if (args_in_dag->getArgName(i)->getAsString() == "\"input\"")
        input_idx = i;
    }
    if (CheckTypeConstraints(args_in_dag->getArg(input_idx), required_types,
                             per_axis)) {
      std::string op_name = op.getCppClassName().str();

      if (per_axis) {
        std::string op_extra_declaration = op.getExtraClassDeclaration().str();
        bool per_axis_support = std::regex_match(
            absl::StrReplaceAll(op_extra_declaration, {{"\n", " "}}),
            per_channel_support_regex);
        if (per_axis_support) result.emplace_back(op_name);
      } else {
        result.emplace_back(op_name);
      }
    }
  }
}

void EmitStaticInt8PerAxisQuantOp(std::vector<Record *> &defs,
                                  raw_ostream &os) {
  os.indent(0)
      << "const std::set<std::string> &ExportStaticInt8PerAxisSpec() {\n";
  os.indent(2) << "static const std::set<std::string> * result =\n";
  os.indent(4) << "new std::set<std::string>({\n";

  std::vector<std::string> result;
  GenerateStaticQuantOp(defs, result, true, true);

  for (const auto &op_name : result) {
    os.indent(6) << "\"" << op_name << "\",\n";
  }

  os.indent(4) << "});";
  os.indent(2) << "return *result;\n";
  os.indent(0) << "}\n";
}

void EmitStaticInt8PerTensorQuantOp(std::vector<Record *> &defs,
                                    raw_ostream &os) {
  os.indent(0)
      << "const std::set<std::string> &ExportStaticInt8PerTensorSpec() {\n";
  os.indent(2) << "static const std::set<std::string> * result =\n";
  os.indent(4) << "new std::set<std::string>({\n";

  std::vector<std::string> result;
  GenerateStaticQuantOp(defs, result, true, false);

  for (const auto &op_name : result) {
    os.indent(6) << "\"" << op_name << "\",\n";
  }

  os.indent(4) << "});";
  os.indent(2) << "return *result;\n";
  os.indent(0) << "}\n";
}

void EmitStaticUInt8PerAxisQuantOp(std::vector<Record *> &defs,
                                   raw_ostream &os) {
  os.indent(0)
      << "const std::set<std::string> &ExportStaticUInt8PerAxisSpec() {\n";
  os.indent(2) << "static const std::set<std::string> * result =\n";
  os.indent(4) << "new std::set<std::string>({\n";

  std::vector<std::string> result;
  GenerateStaticQuantOp(defs, result, false, true);

  for (const auto &op_name : result) {
    os.indent(6) << "\"" << op_name << "\",\n";
  }

  os.indent(4) << "});";
  os.indent(2) << "return *result;\n";
  os.indent(0) << "}\n";
}

void EmitStaticUInt8PerTensorQuantOp(std::vector<Record *> &defs,
                                     raw_ostream &os) {
  os.indent(0)
      << "const std::set<std::string> &ExportStaticUInt8PerTensorSpec() {\n";
  os.indent(2) << "static const std::set<std::string> * result =\n";
  os.indent(4) << "new std::set<std::string>({\n";

  std::vector<std::string> result;
  GenerateStaticQuantOp(defs, result, false, false);

  for (const auto &op_name : result) {
    os.indent(6) << "\"" << op_name << "\",\n";
  }

  os.indent(4) << "});";
  os.indent(2) << "return *result;\n";
  os.indent(0) << "}\n";
}

void EmitStaticQuantOp(std::vector<Record *> &defs, raw_ostream *ostream) {
  raw_ostream &os = *ostream;
  llvm::sort(defs, LessRecord());

  EmitStaticInt8PerAxisQuantOp(defs, os);
  EmitStaticInt8PerTensorQuantOp(defs, os);
  EmitStaticUInt8PerAxisQuantOp(defs, os);
  EmitStaticUInt8PerTensorQuantOp(defs, os);
}

static bool TFLiteOpCoverageSpecWritersMain(raw_ostream &os,
                                            RecordKeeper &records) {
  std::vector<Record *> op_defs = records.getAllDerivedDefinitions("TFL_Op");
  EmitStaticQuantOp(op_defs, &os);
  EmitDynamicRangeOp(op_defs, &os);
  EmitSparseOp(op_defs, &os);
  return false;
}

int main(int argc, char **argv) {
  llvm::InitLLVM y(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv);
  return TableGenMain(argv[0], &TFLiteOpCoverageSpecWritersMain);
}
