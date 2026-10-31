#include <qdb/kernel/column_value.h>
#include <qdb/kernel/gen.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/location.h>
#include <qumir/parser/operator.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace NQdb::NKernel {

int64_t FixedValueWidth(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    if (HasBinIntLayout(type)) {
        return 16;
    }
    const auto valueType = PhysicalValueType(type);
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return integer.Cast()->BitWidth() / 8;
    }
    if (auto floating = TMaybeType<TFloatType>(valueType)) {
        return floating.Cast()->BitWidth() / 8;
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return 1;
    }
    return 0;
}

TColumnValueAst BuildColumnValueAst(
    const std::string& columnName,
    const std::string& rowName,
    const std::string& temporaryPrefix,
    const NQumir::NAst::TTypePtr& logicalType,
    const NQumir::NAst::TTypePtr& stringViewType,
    bool needValue)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i32Type = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value, TTypePtr type = nullptr) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = type ? std::move(type) : i64Type;
        return result;
    };
    auto field = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(loc, ident(columnName), name);
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto cast = [&](TExprPtr value, TTypePtr type) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(value), std::move(type));
    };
    auto index = [&](TExprPtr collection, TExprPtr offset) -> TExprPtr {
        return std::make_shared<TIndexExpr>(
            loc, std::move(collection), std::move(offset));
    };

    const bool nullable = IsNullableType(logicalType);
    auto valueType = nullable ? UnwrapNullableType(logicalType) : logicalType;
    // A decimal loads as its storage: a BinInt, or the plain i64 of p <= 18.
    if (auto decimal = DecimalSpecOfValueType(valueType)) {
        valueType = DecimalStorageType(*decimal);
    }
    const bool isBinIntValue = IsBinIntStorageType(valueType);
    auto type = isBinIntValue ? BinIntStorageType() : UnwrapNamedType(valueType);
    const std::string validName = temporaryPrefix + "_valid";
    const std::string valueName = temporaryPrefix + "_value";
    std::vector<TExprPtr> commonSetup;
    TExprPtr validity;
    if (nullable) {
        auto maskIsNull = binary("==", cast(field("Mask"), i64Type), number(0));
        auto maskBit = std::make_shared<TCallExpr>(loc, ident("bitoff"),
            std::vector<TExprPtr>{field("Mask"), ident(rowName), field("MaskBitOffset")});
        auto isValid = binary("||", std::move(maskIsNull), std::move(maskBit));
        commonSetup = {
            std::make_shared<TVarStmt>(loc, validName, boolType),
            std::make_shared<TAssignExpr>(loc, validName, std::move(isValid)),
        };
        validity = ident(validName);
    } else {
        validity = number(true, boolType);
    }
    if (!needValue) {
        return {
            .Setup = std::move(commonSetup),
            .IsValid = std::move(validity),
        };
    }

    auto zeroBinInt = [&]() -> TExprPtr {
        auto zero = std::make_shared<TNumberExpr>(loc, int64_t{0});
        zero->Type = std::make_shared<TIntegerType>(TIntegerType::I128);
        return std::make_shared<TStructConstructExpr>(loc, BinIntStorageType(),
            std::vector<TExprPtr>{std::move(zero)},
            std::vector<std::string>{"V"});
    };

    if (TMaybeType<TIntegerType>(type) || TMaybeType<TFloatType>(type) ||
        isBinIntValue)
    {
        auto storageType = isBinIntValue ? BinIntStorageType() : valueType;
        auto pointerType = std::make_shared<TPointerType>(storageType);
        auto data = cast(cast(field("Data"), i64Type), pointerType);
        const std::string dataName = temporaryPrefix + "_data";
        if (!nullable) {
            return {
                .Setup = {
                    std::make_shared<TVarStmt>(loc, dataName, pointerType),
                    std::make_shared<TAssignExpr>(loc, dataName, std::move(data)),
                },
                .Value = index(ident(dataName), ident(rowName)),
                .IsValid = std::move(validity),
                .ValueType = storageType,
            };
        }
        commonSetup.insert(commonSetup.end(), {
            std::make_shared<TVarStmt>(loc, dataName, pointerType),
            std::make_shared<TVarStmt>(loc, valueName, storageType),
            std::make_shared<TAssignExpr>(loc, valueName,
                isBinIntValue ? zeroBinInt() : cast(number(0), storageType)),
            std::make_shared<TIfExpr>(loc, ident(validName),
                std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{
                    std::make_shared<TAssignExpr>(loc, dataName,
                        std::move(data)),
                    std::make_shared<TAssignExpr>(loc, valueName,
                        index(ident(dataName), ident(rowName))),
                }), nullptr),
        });
        return {
            .Setup = std::move(commonSetup),
            .Value = ident(valueName),
            .IsValid = std::move(validity),
            .ValueType = storageType,
        };
    }
    if (TMaybeType<TBoolType>(type)) {
        auto data = cast(cast(field("Data"), i64Type), ptrU8Type);
        auto value = std::make_shared<TCallExpr>(loc, ident("bitoff"),
            std::vector<TExprPtr>{
                std::move(data), ident(rowName), field("DataBitOffset")});
        if (!nullable) {
            return {
                .Setup = {},
                .Value = std::move(value),
                .IsValid = std::move(validity),
                .ValueType = valueType,
            };
        }
        commonSetup.insert(commonSetup.end(), {
            std::make_shared<TVarStmt>(loc, valueName, valueType),
            std::make_shared<TAssignExpr>(loc, valueName,
                std::make_shared<TNumberExpr>(loc, false)),
            std::make_shared<TIfExpr>(loc, ident(validName),
                std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{
                    std::make_shared<TAssignExpr>(loc, valueName,
                        std::move(value)),
                }), nullptr),
        });
        return {
            .Setup = std::move(commonSetup),
            .Value = ident(valueName),
            .IsValid = std::move(validity),
            .ValueType = valueType,
        };
    }
    if (TMaybeType<TStringType>(type)) {
        auto namedStringView = AsNamed("StringView", stringViewType);
        const std::string offsets32Name = temporaryPrefix + "_offsets32";
        const std::string offsets64Name = temporaryPrefix + "_offsets64";
        const std::string startName = temporaryPrefix + "_start";
        const std::string endName = temporaryPrefix + "_end";
        auto ptrI32Type = std::make_shared<TPointerType>(i32Type);
        auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
        std::vector<TExprPtr> offsetSetup = {
            std::make_shared<TVarStmt>(loc, offsets32Name, ptrI32Type),
            std::make_shared<TVarStmt>(loc, offsets64Name, ptrI64Type),
            std::make_shared<TVarStmt>(loc, startName, i64Type),
            std::make_shared<TVarStmt>(loc, endName, i64Type),
        };
        commonSetup.insert(commonSetup.end(),
            std::make_move_iterator(offsetSetup.begin()),
            std::make_move_iterator(offsetSetup.end()));
        auto buildOffsetsBranch = [&](const std::string& offsetsName) -> TExprPtr {
            auto start = binary("-",
                cast(index(ident(offsetsName), ident(rowName)), i64Type),
                cast(index(ident(offsetsName), number(0)), i64Type));
            auto end = binary("-",
                cast(index(ident(offsetsName),
                    binary("+", ident(rowName), number(1))), i64Type),
                cast(index(ident(offsetsName), number(0)), i64Type));
            return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{
                std::make_shared<TAssignExpr>(loc, startName, std::move(start)),
                std::make_shared<TAssignExpr>(loc, endName, std::move(end)),
            });
        };
        std::vector<TExprPtr> loadValue = {
            std::make_shared<TAssignExpr>(loc, offsets32Name,
                cast(cast(field("Offsets"), i64Type), ptrI32Type)),
            std::make_shared<TAssignExpr>(loc, offsets64Name,
                cast(cast(field("Offsets"), i64Type), ptrI64Type)),
            std::make_shared<TIfExpr>(loc,
                binary("==", field("OffsetWidth"), number(4, u8Type)),
                buildOffsetsBranch(offsets32Name),
                buildOffsetsBranch(offsets64Name)),
        };
        auto dataAddress = binary("+", cast(field("Data"), i64Type),
            ident(startName));
        auto data = cast(std::move(dataAddress), ptrU8Type);
        auto size = binary("-", ident(endName), ident(startName));
        auto value = std::make_shared<TStructConstructExpr>(loc, namedStringView,
            std::vector<TExprPtr>{std::move(data), std::move(size)});
        if (!nullable) {
            commonSetup.insert(commonSetup.end(),
                std::make_move_iterator(loadValue.begin()),
                std::make_move_iterator(loadValue.end()));
            return {
                .Setup = std::move(commonSetup),
                .Value = std::move(value),
                .IsValid = std::move(validity),
                .ValueType = std::move(namedStringView),
            };
        }
        commonSetup.insert(commonSetup.begin() + 2, {
            std::make_shared<TVarStmt>(loc, valueName, namedStringView),
            std::make_shared<TAssignExpr>(loc, valueName,
                std::make_shared<TStructConstructExpr>(loc, namedStringView,
                    std::vector<TExprPtr>{
                        cast(number(0), ptrU8Type), number(0)})),
        });
        loadValue.push_back(std::make_shared<TAssignExpr>(
            loc, valueName, std::move(value)));
        commonSetup.push_back(std::make_shared<TIfExpr>(loc, ident(validName),
            std::make_shared<TBlockExpr>(loc, std::move(loadValue)), nullptr));
        return {
            .Setup = std::move(commonSetup),
            .Value = ident(valueName),
            .IsValid = std::move(validity),
            .ValueType = std::move(namedStringView),
        };
    }

    throw std::invalid_argument(
        "BuildColumnValueAst: unsupported logical type " +
        (logicalType ? logicalType->ToString() : std::string("<null>")));
}

} // namespace NQdb::NKernel
