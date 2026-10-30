#include <qdb/io/parquet/row_group_predicate.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/utils/module_path.h>

#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

namespace {

constexpr const char* EvalFn = "__row_group_predicate__";

TVmFrontendContext& RowGroupVmContext() {
    static TVmFrontendContext context({
        NUtils::ModuleFile("row_group_interval.oz"),
    });
    return context;
}

enum class ENumberKind {
    Invalid,
    Signed,
    Unsigned,
    Float,
    // The unscaled i128 of a DECIMAL; Scale places its decimal point.
    Decimal,
};

struct TNumberInfo {
    ENumberKind Kind = ENumberKind::Invalid;
    int BitWidth = 0;
    int32_t Scale = 0;

    explicit operator bool() const {
        return Kind != ENumberKind::Invalid;
    }
};

TNumberInfo NumberInfo(const TTypePtr& type) {
    if (auto decimal = DecimalSpecOfValueType(UnwrapNullableType(type))) {
        return {.Kind = ENumberKind::Decimal, .BitWidth = 128, .Scale = decimal->Scale};
    }
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return {
            .Kind = integer.Cast()->IsSigned()
                ? ENumberKind::Signed
                : ENumberKind::Unsigned,
            .BitWidth = integer.Cast()->BitWidth(),
        };
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return {.Kind = ENumberKind::Float, .BitWidth = 64};
    }
    return {};
}

TTypePtr PrimitiveType(ENumberKind kind) {
    switch (kind) {
        case ENumberKind::Signed:
            return std::make_shared<TIntegerType>(TIntegerType::I64);
        case ENumberKind::Unsigned:
            return std::make_shared<TIntegerType>(TIntegerType::U64);
        case ENumberKind::Float:
            return std::make_shared<TFloatType>();
        case ENumberKind::Decimal:
            return std::make_shared<TIntegerType>(TIntegerType::I128);
        case ENumberKind::Invalid:
            return nullptr;
    }
    return nullptr;
}

constexpr __int128 Int128Max =
    static_cast<__int128>(~static_cast<unsigned __int128>(0) >> 1);

__int128 Pow10(int32_t digits) {
    __int128 factor = 1;
    for (int32_t i = 0; i < digits; ++i) {
        factor *= 10;
    }
    return factor;
}

// The unscaled value moved to a larger scale, or nullopt when it leaves i128.
std::optional<__int128> Rescaled(__int128 unscaled, int32_t digits) {
    if (digits < 0 || digits > MaxDecimalPrecision) {
        return std::nullopt;
    }
    const __int128 factor = Pow10(digits);
    const __int128 limit = Int128Max / factor;
    if (unscaled > limit || unscaled < -limit) {
        return std::nullopt;
    }
    return unscaled * factor;
}

TExprPtr Ident(std::string name) {
    return std::make_shared<TIdentExpr>(NQumir::TLocation{}, std::move(name));
}

TExprPtr Call(std::string name, std::vector<TExprPtr> args = {}) {
    return std::make_shared<TCallExpr>(
        NQumir::TLocation{}, Ident(std::move(name)), std::move(args));
}

TExprPtr I64(int64_t value) {
    return std::make_shared<TNumberExpr>(NQumir::TLocation{}, value);
}

TExprPtr U64(uint64_t value) {
    auto out = std::make_shared<TNumberExpr>(
        NQumir::TLocation{}, std::bit_cast<int64_t>(value));
    out->Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    return out;
}

TExprPtr F64(double value) {
    return std::make_shared<TNumberExpr>(NQumir::TLocation{}, value);
}

// The VM takes 64-bit arguments; an i128 travels as two words.
TExprPtr I128(__int128 value) {
    const auto bits = static_cast<unsigned __int128>(value);
    return Call("rg_i128_words", {
        I64(static_cast<int64_t>(static_cast<uint64_t>(bits))),
        I64(static_cast<int64_t>(static_cast<uint64_t>(bits >> 64))),
    });
}

struct TParamColumn {
    size_t Column;
    bool Wide;
};

std::string ParamName(size_t column, std::string_view suffix) {
    return "__rg_" + std::to_string(column) + "_" + std::string(suffix);
}

TExprPtr Bound(size_t column, std::string_view bound, const TNumberInfo& info) {
    if (info.Kind != ENumberKind::Decimal) {
        return Ident(ParamName(column, bound));
    }
    return Call("rg_i128_words", {
        Ident(ParamName(column, std::string(bound) + "_lo")),
        Ident(ParamName(column, std::string(bound) + "_hi")),
    });
}

struct TLoweredNumber {
    TExprPtr Expr;
    TNumberInfo Info;

    explicit operator bool() const {
        return Expr != nullptr && static_cast<bool>(Info);
    }
};

class TPredicateLowerer {
public:
    TPredicateLowerer(const TSchema& schema, std::string_view sourceAlias)
        : Schema_(schema)
        , SourceAlias_(sourceAlias)
    {
        for (size_t i = 0; i < schema.Columns.size(); ++i) {
            const std::string bare(schema.Columns[i].Name);
            BareColumns_.emplace(bare, i);
            if (!SourceAlias_.empty()) {
                QualifiedColumns_.emplace(SourceAlias_ + "." + bare, i);
            }
        }
    }

    TExprPtr LowerTruth(const TExprPtr& expr) {
        if (!expr) {
            return UnknownTruth();
        }
        if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
            const auto op = binary.Cast()->Operator.ToString();
            if (op == "&&" || op == "||") {
                return Call(op == "&&" ? "rg_truth_and" : "rg_truth_or", {
                    LowerTruth(binary.Cast()->Left),
                    LowerTruth(binary.Cast()->Right),
                });
            }
            if (op == "==" || op == "!=" || op == "<" || op == "<=" ||
                op == ">" || op == ">=")
            {
                return LowerComparison(op, binary.Cast()->Left, binary.Cast()->Right);
            }
            return UnknownTruth();
        }
        if (auto unary = TMaybeNode<TUnaryExpr>(expr)) {
            if (unary.Cast()->Operator == "!") {
                return Call("rg_truth_not", {LowerTruth(unary.Cast()->Operand)});
            }
            return UnknownTruth();
        }
        if (auto number = TMaybeNode<TNumberExpr>(expr)) {
            auto boolType = TMaybeType<TBoolType>(UnwrapNullableType(number.Cast()->Type));
            if (boolType) {
                return Call(number.Cast()->IntValue
                    ? "rg_truth_true"
                    : "rg_truth_false");
            }
            return UnknownTruth();
        }
        if (auto call = TMaybeNode<TCallExpr>(expr)) {
            auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee);
            if (!callee) {
                return UnknownTruth();
            }
            const auto& name = callee.Cast()->Name;
            if (name == "qdb_is_true" && call.Cast()->Args.size() == 1) {
                return Call("rg_truth_is_true", {LowerTruth(call.Cast()->Args.front())});
            }
            if (name == "qdb_is_null" && call.Cast()->Args.size() == 1) {
                auto value = LowerNumber(call.Cast()->Args.front(), std::nullopt);
                return value
                    ? Call("rg_is_null", {std::move(value.Expr)})
                    : UnknownTruth();
            }
        }
        return UnknownTruth();
    }

    std::vector<TParam> Params() const {
        std::vector<TParam> out;
        auto param = [&](std::string name, TTypePtr type) {
            out.push_back(std::make_shared<TVarStmt>(
                NQumir::TLocation{}, std::move(name), std::move(type)));
        };
        for (size_t column = 0; column < Schema_.Columns.size(); ++column) {
            auto info = NumberInfo(Schema_.Columns[column].Type);
            if (!info) {
                continue;
            }
            auto word = std::make_shared<TIntegerType>(TIntegerType::I64);
            if (info.Kind == ENumberKind::Decimal) {
                for (const char* bound : {"lo_lo", "lo_hi", "hi_lo", "hi_hi"}) {
                    param(ParamName(column, bound), word);
                }
            } else {
                auto type = PrimitiveType(info.Kind);
                param(ParamName(column, "lo"), type);
                param(ParamName(column, "hi"), type);
            }
            param(ParamName(column, "flags"), word);
        }
        return out;
    }

    std::vector<TParamColumn> ParamColumns() const {
        std::vector<TParamColumn> out;
        for (size_t column = 0; column < Schema_.Columns.size(); ++column) {
            if (auto info = NumberInfo(Schema_.Columns[column].Type)) {
                out.push_back({column, info.Kind == ENumberKind::Decimal});
            }
        }
        return out;
    }

private:
    TExprPtr UnknownTruth() const {
        return Call("rg_truth_unknown");
    }

    std::optional<size_t> ResolveColumn(const std::string& name) const {
        if (auto it = QualifiedColumns_.find(name); it != QualifiedColumns_.end()) {
            return it->second;
        }
        if (auto it = BareColumns_.find(name); it != BareColumns_.end()) {
            return it->second;
        }
        // Qualified identifiers from an already-annotated source are safe to
        // resolve by their final component because this evaluator is attached
        // only to a predicate directly above one source.
        if (auto dot = name.rfind('.'); dot != std::string::npos) {
            if (auto it = BareColumns_.find(name.substr(dot + 1));
                it != BareColumns_.end())
            {
                return it->second;
            }
        }
        return std::nullopt;
    }

    // Moves a decimal range to a larger scale; the kernel does the same
    // (DecimalToDecimal) and never scales down.
    TLoweredNumber Rescale(TLoweredNumber value, TNumberInfo target) {
        if (value.Info.Scale > target.Scale) {
            return {};
        }
        if (value.Info.Scale < target.Scale) {
            value.Expr = Call("rg_i128_scale", {
                std::move(value.Expr),
                I128(Pow10(target.Scale - value.Info.Scale)),
            });
        }
        value.Info = target;
        return value;
    }

    TLoweredNumber Convert(TLoweredNumber value, TNumberInfo target) {
        if (!value || !target) {
            return {};
        }
        if (value.Info.Kind == ENumberKind::Decimal &&
            target.Kind == ENumberKind::Decimal)
        {
            return Rescale(std::move(value), target);
        }
        if (value.Info.Kind == target.Kind) {
            value.Info = target;
            return value;
        }
        if (target.Kind == ENumberKind::Decimal) {
            // A DECIMAL beside a DOUBLE column compares as DOUBLE in the
            // kernel; that pair stays unknown here.
            const char* widen = value.Info.Kind == ENumberKind::Signed
                ? "rg_i128_from_i64"
                : value.Info.Kind == ENumberKind::Unsigned
                    ? "rg_i128_from_u64"
                    : nullptr;
            if (!widen) {
                return {};
            }
            return Rescale({
                .Expr = Call(widen, {std::move(value.Expr)}),
                .Info = {.Kind = ENumberKind::Decimal, .BitWidth = 128},
            }, target);
        }
        if (target.Kind == ENumberKind::Float) {
            if (value.Info.Kind == ENumberKind::Signed) {
                return {
                    .Expr = Call("rg_f64_from_i64", {std::move(value.Expr)}),
                    .Info = target,
                };
            }
            if (value.Info.Kind == ENumberKind::Unsigned) {
                return {
                    .Expr = Call("rg_f64_from_u64", {std::move(value.Expr)}),
                    .Info = target,
                };
            }
        }
        return {};
    }

    // A literal beside a DECIMAL as the kernel types it: a fractional literal
    // is a decimal of its own digits, an integer is DECIMAL(w, 0); both meet
    // the target scale exactly or stay unknown.
    TLoweredNumber LowerDecimalLiteral(
        const TExprPtr& expr,
        const std::shared_ptr<TNumberExpr>& number,
        bool negative,
        TNumberInfo target)
    {
        std::optional<__int128> unscaled;
        if (number->IsFloat()) {
            if (auto literal = NKernel::DecimalLiteralOf(expr)) {
                unscaled = Rescaled(
                    literal->Unscaled, target.Scale - literal->Spec.Scale);
            }
        } else {
            unscaled = Rescaled(
                negative ? -static_cast<__int128>(number->IntValue)
                    : static_cast<__int128>(number->IntValue),
                target.Scale);
        }
        if (!unscaled) {
            return {};
        }
        return {
            .Expr = Call("rg_const", {I128(*unscaled)}),
            .Info = target,
        };
    }

    TLoweredNumber LowerLiteral(
        const std::shared_ptr<TNumberExpr>& number,
        std::optional<TNumberInfo> expected)
    {
        TNumberInfo target = expected.value_or(NumberInfo(number->Type));
        if (!target) {
            return {};
        }
        if (target.Kind == ENumberKind::Decimal) {
            return LowerDecimalLiteral(number, number, false, target);
        }
        TExprPtr literal;
        switch (target.Kind) {
            case ENumberKind::Float:
                literal = F64(number->IsFloat()
                    ? number->FloatValue
                    : static_cast<double>(number->IntValue));
                break;
            case ENumberKind::Signed:
                if (number->IsFloat()) {
                    return {};
                }
                literal = I64(number->IntValue);
                break;
            case ENumberKind::Unsigned:
                if (number->IsFloat() || number->IntValue < 0) {
                    return {};
                }
                literal = U64(static_cast<uint64_t>(number->IntValue));
                break;
            case ENumberKind::Decimal:
            case ENumberKind::Invalid:
                return {};
        }
        return {
            .Expr = Call("rg_const", {std::move(literal)}),
            .Info = target,
        };
    }

    TLoweredNumber LowerIdentifier(
        const std::shared_ptr<TIdentExpr>& ident,
        std::optional<TNumberInfo> expected)
    {
        auto column = ResolveColumn(ident->Name);
        if (!column) {
            return {};
        }
        auto info = NumberInfo(Schema_.Columns[*column].Type);
        if (!info) {
            return {};
        }
        auto value = TLoweredNumber{
            .Expr = Call("rg_range", {
                Bound(*column, "lo", info),
                Bound(*column, "hi", info),
                Ident(ParamName(*column, "flags")),
            }),
            .Info = info,
        };
        return expected ? Convert(std::move(value), *expected) : value;
    }

    static std::pair<int64_t, int64_t> SignedLimits(int bits) {
        if (bits >= 64) {
            return {
                std::numeric_limits<int64_t>::min(),
                std::numeric_limits<int64_t>::max(),
            };
        }
        const int64_t high = (int64_t{1} << (bits - 1)) - 1;
        return {-high - 1, high};
    }

    static uint64_t UnsignedHigh(int bits) {
        return bits >= 64
            ? std::numeric_limits<uint64_t>::max()
            : (uint64_t{1} << bits) - 1;
    }

    TNumberInfo ExpressionNumberInfo(const TExprPtr& expr) const {
        if (!expr) {
            return {};
        }
        if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
            if (auto column = ResolveColumn(ident.Cast()->Name)) {
                return NumberInfo(Schema_.Columns[*column].Type);
            }
            return {};
        }
        return NumberInfo(expr->Type);
    }

    TLoweredNumber LowerArithmetic(
        const std::string& op,
        const TExprPtr& leftExpr,
        const TExprPtr& rightExpr,
        TNumberInfo native,
        std::optional<TNumberInfo> expected)
    {
        if (!native) {
            return {};
        }
        auto left = LowerNumber(leftExpr, native);
        auto right = LowerNumber(rightExpr, native);
        if (!left || !right) {
            return {};
        }
        TExprPtr result;
        if (native.Kind == ENumberKind::Signed) {
            auto [low, high] = SignedLimits(native.BitWidth);
            const std::string fn = op == "+" ? "rg_i64_add"
                : op == "-" ? "rg_i64_sub"
                : "rg_i64_mul";
            result = Call(fn, {
                std::move(left.Expr), std::move(right.Expr), I64(low), I64(high),
            });
        } else if (native.Kind == ENumberKind::Unsigned) {
            const std::string fn = op == "+" ? "rg_u64_add"
                : op == "-" ? "rg_u64_sub"
                : "rg_u64_mul";
            std::vector<TExprPtr> args{
                std::move(left.Expr), std::move(right.Expr),
            };
            if (op != "-") {
                args.push_back(U64(UnsignedHigh(native.BitWidth)));
            }
            result = Call(fn, std::move(args));
        } else if (native.Kind == ENumberKind::Float) {
            const std::string fn = op == "+" ? "rg_f64_add"
                : op == "-" ? "rg_f64_sub"
                : "rg_f64_mul";
            result = Call(fn, {std::move(left.Expr), std::move(right.Expr)});
        } else {
            return {};
        }
        TLoweredNumber value{.Expr = std::move(result), .Info = native};
        return expected ? Convert(std::move(value), *expected) : value;
    }

    TLoweredNumber LowerNumber(
        const TExprPtr& expr,
        std::optional<TNumberInfo> expected)
    {
        if (!expr) {
            return {};
        }
        if (auto number = TMaybeNode<TNumberExpr>(expr)) {
            return LowerLiteral(number.Cast(), expected);
        }
        if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
            return LowerIdentifier(ident.Cast(), expected);
        }
        if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
            const auto op = binary.Cast()->Operator.ToString();
            if (op != "+" && op != "-" && op != "*") {
                return {};
            }
            auto native = ExpressionNumberInfo(expr);
            if (!native && expected) {
                native = *expected;
            }
            return LowerArithmetic(
                op, binary.Cast()->Left, binary.Cast()->Right, native, expected);
        }
        if (auto unary = TMaybeNode<TUnaryExpr>(expr)) {
            if (unary.Cast()->Operator != "-") {
                return {};
            }
            if (expected && expected->Kind == ENumberKind::Decimal) {
                auto number = TMaybeNode<TNumberExpr>(unary.Cast()->Operand);
                return number
                    ? LowerDecimalLiteral(expr, number.Cast(), true, *expected)
                    : TLoweredNumber{};
            }
            auto native = ExpressionNumberInfo(expr);
            if (!native && expected) {
                native = *expected;
            }
            auto operand = LowerNumber(unary.Cast()->Operand, native);
            if (!operand) {
                return {};
            }
            TExprPtr result;
            if (native.Kind == ENumberKind::Signed) {
                auto [low, high] = SignedLimits(native.BitWidth);
                result = Call("rg_i64_neg", {
                    std::move(operand.Expr), I64(low), I64(high),
                });
            } else if (native.Kind == ENumberKind::Float) {
                result = Call("rg_f64_neg", {std::move(operand.Expr)});
            } else {
                return {};
            }
            TLoweredNumber value{.Expr = std::move(result), .Info = native};
            return expected ? Convert(std::move(value), *expected) : value;
        }
        // Casts and scalar calls intentionally remain unknown in v1.
        return {};
    }

    // The scale at which an operand meets a DECIMAL in the kernel: its own
    // for a decimal or a fractional literal, zero for an integer; a DOUBLE
    // column or a literal of more than 18 digits cannot (nullopt).
    std::optional<int32_t> DecimalOperandScale(const TExprPtr& expr) const {
        const auto info = ExpressionNumberInfo(expr);
        switch (info.Kind) {
            case ENumberKind::Decimal:
                return info.Scale;
            case ENumberKind::Signed:
            case ENumberKind::Unsigned:
                return 0;
            case ENumberKind::Float:
                if (auto literal = NKernel::DecimalLiteralOf(expr)) {
                    return literal->Spec.Scale;
                }
                return std::nullopt;
            case ENumberKind::Invalid:
                return std::nullopt;
        }
        return std::nullopt;
    }

    TNumberInfo ComparisonKind(const TExprPtr& left, const TExprPtr& right) const {
        if (ExpressionNumberInfo(left).Kind == ENumberKind::Decimal ||
            ExpressionNumberInfo(right).Kind == ENumberKind::Decimal)
        {
            const auto leftScale = DecimalOperandScale(left);
            const auto rightScale = DecimalOperandScale(right);
            if (!leftScale || !rightScale) {
                return {};
            }
            return {
                .Kind = ENumberKind::Decimal,
                .BitWidth = 128,
                .Scale = std::max(*leftScale, *rightScale),
            };
        }
        return NumberInfo(NKernel::EffectiveBinaryNumericType(left, right));
    }

    TExprPtr LowerComparison(
        const std::string& op,
        const TExprPtr& leftExpr,
        const TExprPtr& rightExpr)
    {
        auto kind = ComparisonKind(leftExpr, rightExpr);
        if (!kind) {
            return UnknownTruth();
        }
        auto left = LowerNumber(leftExpr, kind);
        auto right = LowerNumber(rightExpr, kind);
        if (!left || !right) {
            return UnknownTruth();
        }
        const std::string fn = op == "==" ? "rg_cmp_eq"
            : op == "!=" ? "rg_cmp_ne"
            : op == "<" ? "rg_cmp_lt"
            : op == "<=" ? "rg_cmp_le"
            : op == ">" ? "rg_cmp_gt"
            : "rg_cmp_ge";
        return Call(fn, {std::move(left.Expr), std::move(right.Expr)});
    }

private:
    const TSchema& Schema_;
    std::string SourceAlias_;
    std::unordered_map<std::string, size_t> BareColumns_;
    std::unordered_map<std::string, size_t> QualifiedColumns_;
};

} // namespace

struct TRowGroupPredicateEvaluator::TImpl {
    std::unique_ptr<TVmFunction> Function;
    std::vector<TParamColumn> ParamColumns;
};

TRowGroupPredicateEvaluator::TRowGroupPredicateEvaluator(std::unique_ptr<TImpl> impl)
    : Impl_(std::move(impl))
{}

TRowGroupPredicateEvaluator::~TRowGroupPredicateEvaluator() = default;

std::unique_ptr<TRowGroupPredicateEvaluator> TRowGroupPredicateEvaluator::Compile(
    const TExprPtr& predicate,
    const TSchema& schema,
    std::string_view sourceAlias,
    std::ostream* diagnostics)
{
    try {
        auto diagnostic = [&](const std::string& message) {
            if (diagnostics) {
                *diagnostics << "row-group predicate: " << message << '\n';
            }
        };
        TPredicateLowerer lowerer(schema, sourceAlias);
        auto typedPredicate = CloneExpr(predicate);
        std::vector<std::pair<std::string, TTypePtr>> fields;
        fields.reserve(schema.Columns.size() * (sourceAlias.empty() ? 1 : 2));
        for (const auto& column : schema.Columns) {
            fields.emplace_back(std::string(column.Name), column.Type);
            if (!sourceAlias.empty()) {
                fields.emplace_back(
                    std::string(sourceAlias) + "." + std::string(column.Name),
                    column.Type);
            }
        }
        NKernel::AnnotateExprTreeTypes(
            typedPredicate, TStructType(std::move(fields)));
        auto truth = lowerer.LowerTruth(typedPredicate);
        auto params = lowerer.Params();

        NQumir::TLocation loc{};
        auto body = std::make_shared<TBlockExpr>(
            loc,
            std::vector<TExprPtr>{std::make_shared<TReturnExpr>(loc, std::move(truth))});
        if (diagnostics) {
            diagnostic("compiled body " + NQumir::NAst::NCore::PrintAst(body));
        }

        auto compiled = TVmFunction::Compile(RowGroupVmContext(), {
            .NamePrefix = EvalFn,
            .Params = std::move(params),
            .Body = std::move(body),
            .ReturnType =
                std::make_shared<TIntegerType>(TIntegerType::I64),
            .Imports = {"row_group_interval"},
        });
        if (!compiled) {
            diagnostic(compiled.error());
            return nullptr;
        }

        auto impl = std::make_unique<TImpl>();
        impl->Function = std::move(*compiled);
        impl->ParamColumns = lowerer.ParamColumns();
        return std::unique_ptr<TRowGroupPredicateEvaluator>(
            new TRowGroupPredicateEvaluator(std::move(impl)));
    } catch (const std::exception& error) {
        if (diagnostics) {
            *diagnostics << "row-group predicate: " << error.what() << '\n';
        }
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

bool TRowGroupPredicateEvaluator::MayBeTrue(
    std::span<const TRowGroupColumnRange> columns) const noexcept
{
    if (!Impl_ || !Impl_->Function) {
        return true;
    }
    try {
        std::vector<int64_t> args;
        args.reserve(Impl_->ParamColumns.size() * 5);
        auto pushWords = [&](unsigned __int128 bits, bool wide) {
            args.push_back(static_cast<int64_t>(static_cast<uint64_t>(bits)));
            if (wide) {
                args.push_back(
                    static_cast<int64_t>(static_cast<uint64_t>(bits >> 64)));
            }
        };
        for (const auto& [column, wide] : Impl_->ParamColumns) {
            if (column >= columns.size()) {
                return true;
            }
            const auto& range = columns[column];
            pushWords(range.MinValue, wide);
            pushWords(range.MaxValue, wide);
            int64_t flags = 0;
            if (range.HasBounds) flags |= 1;
            if (range.MayBeNull) flags |= 2;
            if (range.MayBeValue) flags |= 4;
            if (range.MayBeNaN) flags |= 8;
            args.push_back(flags);
        }
        auto result = Impl_->Function->EvalRaw(std::move(args));
        if (!result) {
            return true;
        }
        return (*result & 2) != 0;
    } catch (...) {
        return true;
    }
}

} // namespace NQdb
