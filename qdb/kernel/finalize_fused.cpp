#include <qdb/kernel/finalize_fused.h>

#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NQdb {

namespace {

using namespace NQumir::NAst;

std::string KernelPrefix(size_t index) {
  return "__qdb_k" + std::to_string(index) + "_";
}

std::string PrintDecl(const TExprPtr &expr) {
  return NCore::PrintAst(expr, NCore::TPrintOptions{.Pretty = false});
}

void AppendKeyPart(std::string &key, std::string_view part) {
  key += std::to_string(part.size());
  key += ':';
  key += part;
}

std::string TypeKeyString(const TTypePtr &type) {
  return NQumir::NAst::TypeKey(type);
}

void RewriteCalls(const TExprPtr &expr,
                  const std::unordered_map<std::string, std::string> &renames) {
  if (!expr) {
    return;
  }

  if (auto call = TMaybeNode<TCallExpr>(expr)) {
    if (auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee)) {
      if (auto it = renames.find(callee.Cast()->Name); it != renames.end()) {
        callee.Cast()->Name = it->second;
      }
    }
  }

  for (auto *child : expr->MutableChildren()) {
    RewriteCalls(*child, renames);
  }
}

void CollectLocalCalls(const TExprPtr &expr,
                       const std::unordered_set<std::string> &localNames,
                       std::unordered_set<std::string> &calls) {
  if (!expr) {
    return;
  }

  if (auto call = TMaybeNode<TCallExpr>(expr)) {
    if (auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee)) {
      const auto &name = callee.Cast()->Name;
      if (localNames.contains(name)) {
        calls.insert(name);
      }
    }
  }

  for (const auto &child : expr->Children()) {
    CollectLocalCalls(child, localNames, calls);
  }
}

std::string FunctionSignatureKey(const TFunDecl &fun) {
  std::string key;
  key += "generics(";
  for (const auto &generic : fun.GenericParams) {
    key += generic.Name;
    key += ':';
    key += generic.Kind == TGenericParam::EKind::Type ? "type" : "value";
    key += ':';
    key += TypeKeyString(generic.ValueType);
    key += ';';
  }
  key += ")params(";
  for (const auto &param : fun.Params) {
    key += TypeKeyString(param->Type);
    key += ';';
  }
  key += ")ret(";
  key += TypeKeyString(fun.RetType);
  key += ")attrs(";
  key += fun.MangledName;
  key += ';';
  key += fun.OperatorName.value_or("");
  key += ';';
  key += fun.LiteralSuffix.value_or("");
  key += ';';
  key += fun.RequireArgsMaterialization ? "materialize" : "";
  key += ')';
  return key;
}

std::string TypeDeclName(const TTypeDeclStmt &typeDecl) {
  if (auto named = TMaybeType<TNamedType>(typeDecl.Type)) {
    return named.Cast()->Name;
  }
  return {};
}

class TFusedProgramBuilder {
public:
  std::vector<std::string> AddKernel(
      TGeneratedKernel &kernel, size_t index,
      std::unordered_map<std::string, std::string> *outRenames = nullptr) {
    auto block = TMaybeNode<TBlockExpr>(kernel.Ast);
    if (!block) {
      throw std::runtime_error("kernel '" + kernel.Name +
                               "' AST is not a top-level block");
    }

    const std::string prefix = KernelPrefix(index);
    std::unordered_set<TExpr *> includeTypes;
    std::unordered_set<TExpr *> includeFunctions;

    IncludeTypeDecls(*block.Cast(), includeTypes);

    std::unordered_map<std::string, std::string> functionRenames;
    DecideFunctions(*block.Cast(), prefix, kernel.Entrypoints, functionRenames,
                    includeFunctions);
    if (!functionRenames.empty()) {
      RewriteFunctionDeclNames(*block.Cast(), functionRenames);
      RewriteCalls(kernel.Ast, functionRenames);
    }

    AppendIncludedDecls(*block.Cast(), includeTypes, includeFunctions);

    if (outRenames) {
      *outRenames = functionRenames;
    }

    std::vector<std::string> result;
    result.reserve(kernel.Entrypoints.size());
    for (const auto &entrypoint : kernel.Entrypoints) {
      if (auto it = functionRenames.find(entrypoint);
          it != functionRenames.end()) {
        result.push_back(it->second);
      } else {
        result.push_back(entrypoint);
      }
      EntrypointNames_.insert(entrypoint);
    }
    return result;
  }

  TExprPtr Build() {
    std::vector<TExprPtr> stmts;
    stmts.reserve(TypeDecls_.size() + FunctionDecls_.size());
    for (auto &stmt : TypeDecls_) {
      stmts.push_back(std::move(stmt));
    }
    for (auto &stmt : FunctionDecls_) {
      stmts.push_back(std::move(stmt));
    }
    return NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, std::move(stmts));
  }

  size_t TypeDeclCount() const { return TypeDecls_.size(); }

  size_t FunctionDeclCount() const { return FunctionDecls_.size(); }

private:
  // Kernel generators name their types uniquely, so in one query a name is one
  // interned type, which is declared once.
  void IncludeTypeDecls(TBlockExpr &block,
                        std::unordered_set<TExpr *> &includeTypes) {
    for (const auto &stmt : block.Stmts) {
      auto typeDecl = TMaybeNode<TTypeDeclStmt>(stmt);
      if (!typeDecl) {
        continue;
      }

      const std::string name = TypeDeclName(*typeDecl.Cast());
      if (name.empty()) {
        throw std::runtime_error(
            "top-level type declaration has no named type");
      }

      auto [known, inserted] = IncludedTypes_.emplace(name, typeDecl.Cast()->Type);
      if (inserted) {
        includeTypes.insert(stmt.Get());
      } else if (known->second != typeDecl.Cast()->Type) {
        throw std::runtime_error("kernels declare two types named '" + name + "'");
      }
    }
  }

  void DecideFunctions(TBlockExpr &block, const std::string &prefix,
                       const std::vector<std::string> &entrypoints,
                       std::unordered_map<std::string, std::string> &renames,
                       std::unordered_set<TExpr *> &includeFunctions) {
    std::vector<std::string> names;
    std::unordered_map<std::string, std::vector<NQumir::TIntrusivePtr<TFunDecl>>>
        groups;
    for (const auto &stmt : block.Stmts) {
      auto fun = TMaybeNode<TFunDecl>(stmt);
      if (!fun) {
        continue;
      }
      const std::string name = fun.Cast()->Name;
      if (!groups.contains(name)) {
        names.push_back(name);
      }
      groups[name].emplace_back(fun.Cast());
    }

    auto renameNames = FunctionNamesToRename(names, groups, entrypoints);
    for (const auto &component : RenameComponents(names, groups, renameNames)) {
      const std::string key = RenamedComponentKey(component, groups);
      auto known = RenamedComponents_.find(key);
      if (known != RenamedComponents_.end()) {
        renames.insert(known->second.begin(), known->second.end());
        continue;
      }

      std::unordered_map<std::string, std::string> outputs;
      for (const auto &name : component) {
        auto outputName = prefix + name;
        renames.emplace(name, outputName);
        outputs.emplace(name, std::move(outputName));
        for (const auto &fun : groups.at(name)) {
          includeFunctions.insert(fun.Get());
        }
      }
      RenamedComponents_.emplace(key, std::move(outputs));
    }

    for (const auto &name : names) {
      auto &functions = groups[name];
      if (renameNames.contains(name)) {
        continue;
      }

      for (const auto &fun : functions) {
        const std::string signature = FunctionSignatureKey(*fun);
        const std::string key = PrintDecl(fun);
        auto &knownBySignature = Functions_[name];
        auto known = knownBySignature.find(signature);
        if (known == knownBySignature.end()) {
          knownBySignature.emplace(signature, key);
          includeFunctions.insert(fun.Get());
        } else if (known->second != key) {
          throw std::runtime_error("function conflict was not renamed: " +
                                   name);
        }
      }
    }
  }

  std::unordered_set<std::string> FunctionNamesToRename(
      const std::vector<std::string> &names,
      const std::unordered_map<std::string,
                               std::vector<NQumir::TIntrusivePtr<TFunDecl>>> &groups,
      const std::vector<std::string> &entrypoints) const {
    std::unordered_set<std::string> localNames(names.begin(), names.end());
    std::unordered_map<std::string, std::unordered_set<std::string>>
        callsByName;
    for (const auto &name : names) {
      auto &calls = callsByName[name];
      for (const auto &fun : groups.at(name)) {
        CollectLocalCalls(fun, localNames, calls);
      }
    }

    std::unordered_set<std::string> renameNames;
    for (const auto &entrypoint : entrypoints) {
      if (EntrypointNames_.contains(entrypoint)) {
        renameNames.insert(entrypoint);
      }
    }
    for (const auto &name : names) {
      if (MustRenameFunctionGroup(name, groups.at(name))) {
        renameNames.insert(name);
      }
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &name : names) {
        if (renameNames.contains(name)) {
          continue;
        }
        const auto &calls = callsByName.at(name);
        if (std::ranges::any_of(calls, [&](const std::string &callee) {
              return renameNames.contains(callee);
            })) {
          renameNames.insert(name);
          changed = true;
        }
      }
    }

    return renameNames;
  }

  std::vector<std::vector<std::string>> RenameComponents(
      const std::vector<std::string> &names,
      const std::unordered_map<std::string,
                               std::vector<NQumir::TIntrusivePtr<TFunDecl>>> &groups,
      const std::unordered_set<std::string> &renameNames) const {
    std::unordered_set<std::string> localNames(names.begin(), names.end());
    std::unordered_map<std::string, std::unordered_set<std::string>> adjacent;
    for (const auto &name : renameNames) {
      adjacent[name];
      std::unordered_set<std::string> calls;
      for (const auto &fun : groups.at(name)) {
        CollectLocalCalls(fun, localNames, calls);
      }
      for (const auto &callee : calls) {
        if (!renameNames.contains(callee)) {
          continue;
        }
        adjacent[name].insert(callee);
        adjacent[callee].insert(name);
      }
    }

    std::vector<std::vector<std::string>> components;
    std::unordered_set<std::string> seen;
    for (const auto &root : names) {
      if (!renameNames.contains(root) || !seen.insert(root).second) {
        continue;
      }
      std::vector<std::string> component;
      std::vector<std::string> pending{root};
      while (!pending.empty()) {
        auto name = std::move(pending.back());
        pending.pop_back();
        component.push_back(name);
        for (const auto &next : adjacent.at(name)) {
          if (seen.insert(next).second) {
            pending.push_back(next);
          }
        }
      }
      std::ranges::sort(component);
      components.push_back(std::move(component));
    }
    return components;
  }

  std::string RenamedComponentKey(
      const std::vector<std::string> &component,
      const std::unordered_map<
          std::string, std::vector<NQumir::TIntrusivePtr<TFunDecl>>> &groups) const {
    std::string key;
    for (const auto &name : component) {
      AppendKeyPart(key, name);
      std::vector<std::string> variants;
      variants.reserve(groups.at(name).size());
      for (const auto &fun : groups.at(name)) {
        std::string variant;
        AppendKeyPart(variant, FunctionSignatureKey(*fun));
        AppendKeyPart(variant, PrintDecl(fun));
        variants.push_back(std::move(variant));
      }
      std::ranges::sort(variants);
      for (const auto &variant : variants) {
        AppendKeyPart(key, variant);
      }
    }
    return key;
  }

  bool MustRenameFunctionGroup(
      const std::string &name,
      const std::vector<NQumir::TIntrusivePtr<TFunDecl>> &functions) const {
    std::unordered_map<std::string, std::string> local;
    for (const auto &fun : functions) {
      const std::string signature = FunctionSignatureKey(*fun);
      const std::string key = PrintDecl(fun);
      auto localKnown = local.find(signature);
      if (localKnown != local.end() && localKnown->second != key) {
        throw std::runtime_error(
            "one kernel has conflicting overloads for function '" + name + "'");
      }
      local.emplace(signature, key);

      auto byName = Functions_.find(name);
      if (byName == Functions_.end()) {
        continue;
      }
      auto bySignature = byName->second.find(signature);
      if (bySignature != byName->second.end() && bySignature->second != key) {
        return true;
      }
    }
    return false;
  }

  void RewriteFunctionDeclNames(
      TBlockExpr &block,
      const std::unordered_map<std::string, std::string> &renames) {
    for (const auto &stmt : block.Stmts) {
      auto fun = TMaybeNode<TFunDecl>(stmt);
      if (!fun) {
        continue;
      }
      if (auto it = renames.find(fun.Cast()->Name); it != renames.end()) {
        fun.Cast()->Name = it->second;
      }
    }
  }

  void
  AppendIncludedDecls(TBlockExpr &block,
                      const std::unordered_set<TExpr *> &includeTypes,
                      const std::unordered_set<TExpr *> &includeFunctions) {
    for (const auto &stmt : block.Stmts) {
      if (TMaybeNode<TUseExpr>(stmt)) {
        continue;
      }
      if (TMaybeNode<TTypeDeclStmt>(stmt)) {
        if (includeTypes.contains(stmt.Get())) {
          TypeDecls_.push_back(stmt);
        }
      } else if (TMaybeNode<TFunDecl>(stmt)) {
        if (includeFunctions.contains(stmt.Get())) {
          FunctionDecls_.push_back(stmt);
        }
      } else {
        throw std::runtime_error("unsupported top-level kernel AST node '" +
                                 std::string(stmt->NodeName()) + "'");
      }
    }
  }

  std::vector<TExprPtr> TypeDecls_;
  std::vector<TExprPtr> FunctionDecls_;
  std::unordered_map<std::string, TTypePtr> IncludedTypes_;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      Functions_;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      RenamedComponents_;
  std::unordered_set<std::string> EntrypointNames_;
};

} // namespace

std::string MakeKernelDedupKey(const TGeneratedKernel &kernel) {
  std::string key = NCore::PrintAst(kernel.Ast);
  key += "\n# qdb-kernel-entrypoints\n";
  for (const auto &entrypoint : kernel.Entrypoints) {
    key += std::to_string(entrypoint.size());
    key += ':';
    key += entrypoint;
    key += '\n';
  }
  return key;
}

TKernelDedup DeduplicateKernels(
    std::span<TGeneratedKernel> kernels,
    const std::function<bool(const TGeneratedKernel &)> &skip) {
  TKernelDedup out;
  std::unordered_map<std::string, size_t> uniqueByKey;
  for (auto &kernel : kernels) {
    if (!kernel.Slot) {
      throw std::logic_error("FinalizeKernels: kernel '" + kernel.Name + "' has no slot");
    }
    if (skip(kernel)) {
      continue;
    }
    auto [it, inserted] =
        uniqueByKey.emplace(MakeKernelDedupKey(kernel), out.Unique.size());
    if (inserted) {
      out.Unique.push_back(&kernel);
    }
    out.Bindings.emplace_back(&kernel, it->second);
  }
  return out;
}

TFusedProgram
BuildFusedProgram(std::span<TGeneratedKernel *const> uniqueKernels) {
  TFusedProgramBuilder builder;
  TFusedProgram out;
  std::unordered_set<std::string> seen;
  out.UniqueEntrypoints.reserve(uniqueKernels.size());
  out.UniqueRenames.reserve(uniqueKernels.size());
  for (size_t i = 0; i < uniqueKernels.size(); ++i) {
    std::unordered_map<std::string, std::string> renames;
    auto fused = builder.AddKernel(*uniqueKernels[i], i, &renames);
    for (const auto &entrypoint : fused) {
      if (seen.insert(entrypoint).second) {
        out.Entrypoints.push_back(entrypoint);
      }
    }
    out.UniqueEntrypoints.push_back(std::move(fused));
    out.UniqueRenames.push_back(std::move(renames));
  }
  out.Program = builder.Build();
  out.TypeDeclCount = builder.TypeDeclCount();
  out.FunctionDeclCount = builder.FunctionDeclCount();
  return out;
}

} // namespace NQdb
