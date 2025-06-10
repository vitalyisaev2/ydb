#include "collection.h"

#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/providers/common/provider/yql_provider.h>
#include <yql/essentials/utils/log/log.h>

#include <vector>

namespace NYql::NPushdown {

using namespace NNodes;

namespace {

// Build and markup a predicate tree whose leaves are expressions:
//          A  OR  B
//          |      |
//       C AND D   COALESCE(X, Y, Z)
//       |     |            |  |  |
//   Member   Comparation    .....
//
// Each node has flag if it can be pushed entirely,
// Next some tree nodes will be split
class TPredicateMarkup {
    using EFlag = TSettings::EFeatureFlag;

public:
    TPredicateMarkup(const TExprBase& lambdaArg, const TSettings& settings, TExprContext& ctx)
        : LambdaArg(lambdaArg)
        , Settings(settings)
        , Ctx(ctx)
    {}

    void MarkupPredicates(const TExprBase& predicate, TPredicateNode& predicateTree) {
        Cout << "\n\nMarkupPredicates: " << NCommon::ExprToPrettyString(Ctx, predicate.Ref()) << Endl; 

        if (auto coalesce = predicate.Maybe<TCoCoalesce>()) {
            Cout << "MarkupPredicates: TCoCoalesce" << Endl;
            if (Settings.IsEnabled(EFlag::JustPassthroughOperators)) {
                Cout << "  Sub-branch: JustPassthroughOperators enabled" << Endl;
                CollectChildrenPredicates(predicate, predicateTree);
            } else {
                Cout << "  Sub-branch: JustPassthroughOperators disabled" << Endl;
                predicateTree.CanBePushed = CoalesceCanBePushed(coalesce.Cast());
            }
        } else if (auto compare = predicate.Maybe<TCoCompare>()) {
            Cout << "MarkupPredicates: TCoCompare" << Endl;
            predicateTree.CanBePushed = CompareCanBePushed(compare.Cast());
        } else if (auto exists = predicate.Maybe<TCoExists>()) {
            Cout << "MarkupPredicates: TCoExists" << Endl;
            predicateTree.CanBePushed = ExistsCanBePushed(exists.Cast());
        } else if (auto notOp = predicate.Maybe<TCoNot>()) {
            Cout << "MarkupPredicates: TCoNot" << Endl;
            const auto value = notOp.Cast().Value();
            TPredicateNode child(value);
            MarkupPredicates(value, child);
            predicateTree.Op = EBoolOp::Not;
            predicateTree.CanBePushed = child.CanBePushed;
            predicateTree.Children.emplace_back(child);
        } else if (predicate.Maybe<TCoAnd>()) {
            Cout << "MarkupPredicates: TCoAnd" << Endl;
            predicateTree.Op = EBoolOp::And;
            CollectChildrenPredicates(predicate, predicateTree);
        } else if (predicate.Maybe<TCoOr>()) {
            Cout << "MarkupPredicates: TCoOr" << Endl;
            predicateTree.Op = EBoolOp::Or;
            CollectChildrenPredicates(predicate, predicateTree);
        } else if (Settings.IsEnabled(EFlag::LogicalXorOperator) && predicate.Maybe<TCoXor>()) {
            Cout << "MarkupPredicates: TCoXor (LogicalXorOperator enabled)" << Endl;
            predicateTree.Op = EBoolOp::Xor;
            CollectChildrenPredicates(predicate, predicateTree);
        } else if (auto jsonExists = predicate.Maybe<TCoJsonExists>()) {
            Cout << "MarkupPredicates: TCoJsonExists" << Endl;
            predicateTree.CanBePushed = JsonExistsCanBePushed(jsonExists.Cast());
        } else if (Settings.IsEnabled(EFlag::JustPassthroughOperators) && (predicate.Maybe<TCoIf>() || predicate.Maybe<TCoJust>())) {
            Cout << "MarkupPredicates: TCoIf or TCoJust (JustPassthroughOperators enabled)" << Endl;
            CollectChildrenPredicates(predicate, predicateTree);
        } else if (auto sqlIn = predicate.Maybe<TCoSqlIn>()) {
            Cout << "MarkupPredicates: TCoSqlIn" << Endl;
            predicateTree.CanBePushed = SqlInCanBePushed(sqlIn.Cast());
        } else if (predicate.Ref().IsCallable({"IsNotDistinctFrom", "IsDistinctFrom"})) {
            Cout << "MarkupPredicates: IsNotDistinctFrom or IsDistinctFrom" << Endl;
            predicateTree.CanBePushed = IsDistinctCanBePushed(predicate);
        } else if (auto apply = predicate.Maybe<TCoApply>()) {
            Cout << "MarkupPredicates: TCoApply" << Endl;
            predicateTree.CanBePushed = ApplyCanBePushed(apply.Cast());
        } else if (Settings.IsEnabled(EFlag::ExpressionAsPredicate)) {
            Cout << "MarkupPredicates: ExpressionAsPredicate enabled" << Endl;
            predicateTree.CanBePushed = CheckExpressionNodeForPushdown(predicate);
        } else {
            Cout << "MarkupPredicates: Default (no matching predicate type)" << Endl;
            predicateTree.CanBePushed = false;
        }
        Cout << "MarkupPredicates: CanBePushed = " << (predicateTree.CanBePushed ? "true" : "false") << Endl;
    }

    void CollectChildrenPredicates(const TExprBase& node, TPredicateNode& predicateTree) {
        predicateTree.Children.reserve(node.Ref().ChildrenSize());
        predicateTree.CanBePushed = true;
        for (const auto& childNodePtr: node.Ref().Children()) {
            TPredicateNode child(childNodePtr);
            MarkupPredicates(TExprBase(childNodePtr), child);

            predicateTree.Children.emplace_back(child);
            predicateTree.CanBePushed &= child.CanBePushed;
        }
    }

private:
    // Type helpers
    static std::optional<NUdf::EDataSlot> DataSlotFromDataType(const TTypeAnnotationNode* typeAnn) {
        if (!typeAnn || typeAnn->GetKind() != ETypeAnnotationKind::Data) {
            return std::nullopt;
        }
        return typeAnn->Cast<TDataExprType>()->GetSlot();
    }

    static std::optional<NUdf::EDataSlot> DataSlotFromOptionalDataType(const TTypeAnnotationNode* typeAnn) {
        if (typeAnn->GetKind() == ETypeAnnotationKind::Optional) {
            typeAnn = typeAnn->Cast<TOptionalExprType>()->GetItemType();
        }
        return DataSlotFromDataType(typeAnn);
    }

    static const TTypeAnnotationNode* UnwrapExprType(const TTypeAnnotationNode* typeAnn) {
        if (!typeAnn) {
            return nullptr;
        }
        if (const auto typeExpr = typeAnn->Cast<TTypeExprType>()) {
            return typeExpr->GetType();
        }
        return nullptr;
    }

    static bool IsStringType(std::optional<NUdf::EDataSlot> dataSlot) {
        return dataSlot && (IsDataTypeString(*dataSlot) || *dataSlot == NUdf::EDataSlot::JsonDocument);
    }

    static bool IsUtf8Type(std::optional<NUdf::EDataSlot> dataSlot) {
        return dataSlot == NUdf::EDataSlot::Utf8;
    }

    static bool IsDateTimeType(std::optional<NUdf::EDataSlot> dataSlot) {
        return dataSlot && IsDataTypeDateOrTzDateOrInterval(*dataSlot);
    }

    static bool IsUuidType(std::optional<NUdf::EDataSlot> dataSlot) {
        return dataSlot == NUdf::EDataSlot::Uuid;
    }

    static bool IsDecimalType(std::optional<NUdf::EDataSlot> dataSlot) {
        return dataSlot == NUdf::EDataSlot::Decimal;
    }

    static bool IsDyNumberType(std::optional<NUdf::EDataSlot> dataSlot) {
        return dataSlot == NUdf::EDataSlot::DyNumber;
    }

    static bool IsNumericType(std::optional<NUdf::EDataSlot> dataSlot) {
        return dataSlot && IsDataTypeNumeric(*dataSlot);
    }

    static bool IsSignedIntegralType(std::optional<NUdf::EDataSlot> dataSlot) {
        return dataSlot && IsDataTypeSigned(*dataSlot);
    }

    static bool IsUnsignedIntegralType(std::optional<NUdf::EDataSlot> dataSlot) {
        return dataSlot && IsDataTypeUnsigned(*dataSlot);
    }

    static bool IsComparableTypes(const TTypeAnnotationNode* left, const TTypeAnnotationNode* right, bool equality) {
        if (equality) {
            return CanCompare<true>(left, right) != ECompareOptions::Uncomparable;
        }
        return CanCompare<false>(left, right) != ECompareOptions::Uncomparable;
    }

    static bool IsStringExpr(const TExprBase& expr) {
        return IsStringType(DataSlotFromOptionalDataType(expr.Ref().GetTypeAnn()));
    }

    static bool IsUtf8Expr(const TExprBase& expr) {
        return IsUtf8Type(DataSlotFromOptionalDataType(expr.Ref().GetTypeAnn()));
    }

    // Callable helpers
    static bool IsSimpleLikeOperator(const TCoCompare& predicate) {
        // Only cases $A LIKE $B, where $B:
        // "%str", "str%", "%str%"
        return predicate.Maybe<TCoCmpStringContains>()
            || predicate.Maybe<TCoCmpStartsWith>()
            || predicate.Maybe<TCoCmpEndsWith>();
    }

    static std::vector<TExprBase> GetComparisonNodes(const TExprBase& node) {
        std::vector<TExprBase> result;
        if (const auto maybeList = node.Maybe<TExprList>()) {
            const auto nodeList = maybeList.Cast();
            result.reserve(nodeList.Size());
            for (size_t i = 0; i < nodeList.Size(); ++i) {
                result.emplace_back(nodeList.Item(i));
            }
        } else {
            result.emplace_back(node);
        }
        return result;
    }

private:
    // Genric expression checking
    bool IsSupportedDataType(const TCoDataCtor& node) const {
        if (node.Maybe<TCoBool>() ||
            node.Maybe<TCoFloat>() ||
            node.Maybe<TCoDouble>() ||
            node.Maybe<TCoInt8>() ||
            node.Maybe<TCoInt16>() ||
            node.Maybe<TCoInt32>() ||
            node.Maybe<TCoInt64>() ||
            node.Maybe<TCoUint8>() ||
            node.Maybe<TCoUint16>() ||
            node.Maybe<TCoUint32>() ||
            node.Maybe<TCoUint64>()) {
            return true;
        }
        if (Settings.IsEnabled(EFlag::TimestampCtor) && node.Maybe<TCoTimestamp>()) {
            return true;
        }
        if (Settings.IsEnabled(EFlag::StringTypes) && (node.Maybe<TCoUtf8>() || node.Maybe<TCoString>())) {
            return true;
        }
        return false;
    }

    bool IsMemberColumn(const TCoMember& member) const {
        // We allow member acces only for top level predicate argument
        return member.Struct().Raw() == LambdaArg.Raw();
    }

    bool IsMemberColumn(const TExprBase& node) const {
        if (const auto member = node.Maybe<TCoMember>()) {
            return IsMemberColumn(member.Cast());
        }
        return false;
    }

    bool IsSupportedSafeCast(const TCoSafeCast& cast) {
        if (!Settings.IsEnabled(EFlag::CastExpression)) {
            return false;
        }

        const auto targetType = DataSlotFromOptionalDataType(UnwrapExprType(cast.Type().Ref().GetTypeAnn()));
        if (targetType == EDataSlot::Bool || IsNumericType(targetType) || IsStringType(targetType) && Settings.IsEnabled(EFlag::StringTypes)) {
            return CheckExpressionNodeForPushdown(cast.Value());
        }
        return false;
    }

    bool IsSupportedToBytes(const TExprBase& toBytes) {
        if (!Settings.IsEnabled(EFlag::ToBytesFromStringExpressions)) {
            return false;
        }
        if (toBytes.Ref().ChildrenSize() != 1) {
            return false;
        }

        auto toBytesExpr = TExprBase(toBytes.Ref().Child(0));
        if (!IsStringExpr(toBytesExpr)) {
            return false;
        }
        return CheckExpressionNodeForPushdown(toBytesExpr);
    }

    bool IsSupportedToString(const TExprBase& toString) {
        if (!Settings.IsEnabled(EFlag::ToStringFromStringExpressions)) {
            return false;
        }

        if (toString.Ref().ChildrenSize() != 1) {
            return false;
        }

        auto toStringExpr = TExprBase(toString.Ref().Child(0));
        if (!IsStringExpr(toStringExpr)) {
            return false;
        }
        return CheckExpressionNodeForPushdown(toStringExpr);
    }

    bool IsSupportedLambda(const TCoLambda& lambda, ui64 numberArguments) {
        const auto args = lambda.Args();
        if (args.Size() != numberArguments) {
            return false;
        }

        // Add arguments into current context
        for (const auto& argPtr : args.Ref().Children()) {
            YQL_ENSURE(LambdaArguments.insert(argPtr.Get()).second, "Found duplicated lambda argument");
        }

        const bool result = CheckExpressionNodeForPushdown(lambda.Body());

        // Remove arguments from current context
        for (const auto& argPtr : args.Ref().Children()) {
            LambdaArguments.erase(argPtr.Get());
        }

        return result;
    }

    bool IsSupportedFlatMap(const TCoFlatMap& flatMap) {
        if (!Settings.IsEnabled(EFlag::FlatMapOverOptionals)) {
            return false;
        }

        const auto input = flatMap.Input();
        if (!DataSlotFromOptionalDataType(input.Ref().GetTypeAnn())) {
            // Supported only simple flat map over one optional
            return false;
        }
        if (!CheckExpressionNodeForPushdown(input)) {
            return false;
        }

        // Expected exactly one argument for flat map lambda
        return IsSupportedLambda(flatMap.Lambda(), 1);
    }

    bool IsLambdaArgument(const TExprBase& expr) const {
        return LambdaArguments.contains(expr.Raw());
    }

    bool CheckExpressionNodeForPushdown(const TExprBase& node) {
        Cout << "CheckExpressionNodeForPushdown: " << NCommon::ExprToPrettyString(Ctx, node.Ref()) << Endl;

        if (auto maybeSafeCast = node.Maybe<TCoSafeCast>()) {
            Cout << "CheckExpressionNodeForPushdown: TCoSafeCast" << Endl;
            return IsSupportedSafeCast(maybeSafeCast.Cast());
        }
        if (node.Ref().IsCallable({"ToBytes"})) {
            Cout << "CheckExpressionNodeForPushdown: ToBytes" << Endl;
            return IsSupportedToBytes(node);
        }
        if (node.Ref().IsCallable({"ToString"})) {
            Cout << "CheckExpressionNodeForPushdown: ToString" << Endl;
            return IsSupportedToString(node);
        }
        if (auto maybeData = node.Maybe<TCoDataCtor>()) {
            Cout << "CheckExpressionNodeForPushdown: TCoDataCtor" << Endl;
            return IsSupportedDataType(maybeData.Cast());
        }
        if (auto maybeMember = node.Maybe<TCoMember>()) {
            Cout << "CheckExpressionNodeForPushdown: TCoMember" << Endl;
            return IsMemberColumn(maybeMember.Cast());
        }
        if (Settings.IsEnabled(EFlag::JsonQueryOperators) && node.Maybe<TCoJsonQueryBase>()) {
            Cout << "CheckExpressionNodeForPushdown: TCoJsonQueryBase (JsonQueryOperators enabled)" << Endl;
            if (!node.Maybe<TCoJsonValue>()) {
                Cout << "CheckExpressionNodeForPushdown: Not TCoJsonValue, returning false" << Endl;
                return false;
            }

            // Currently we support only simple columns in pushdown
            const auto jsonOp = node.Cast<TCoJsonQueryBase>();
            bool result = jsonOp.Json().Maybe<TCoMember>() && jsonOp.JsonPath().Maybe<TCoUtf8>();
            Cout << "CheckExpressionNodeForPushdown: Checking Json is TCoMember and JsonPath is TCoUtf8, result: " << (result ? "true" : "false") << Endl;
            return result;
        }
        if (node.Maybe<TCoNull>()) {
            Cout << "CheckExpressionNodeForPushdown: TCoNull" << Endl;
            return true;
        }
        if (Settings.IsEnabled(EFlag::ParameterExpression) && node.Maybe<TCoParameter>()) {
            Cout << "CheckExpressionNodeForPushdown: TCoParameter (ParameterExpression enabled)" << Endl;
            return true;
        }
        if (const auto op = node.Maybe<TCoUnaryArithmetic>(); op && Settings.IsEnabled(EFlag::UnaryOperators)) {
            Cout << "CheckExpressionNodeForPushdown: TCoUnaryArithmetic (UnaryOperators enabled)" << Endl;
            return CheckExpressionNodeForPushdown(op.Cast().Arg());
        }
        if (const auto op = node.Maybe<TCoBinaryArithmetic>(); op && Settings.IsEnabled(EFlag::ArithmeticalExpressions)) {
            Cout << "CheckExpressionNodeForPushdown: TCoBinaryArithmetic (ArithmeticalExpressions enabled)" << Endl;
            if (!Settings.IsEnabled(EFlag::DivisionExpressions) && (op.Maybe<TCoDiv>() || op.Maybe<TCoMod>())) {
                Cout << "  Sub-branch: TCoDiv or TCoMod with DivisionExpressions disabled, returning false" << Endl;
                return false;
            }
            bool leftResult = CheckExpressionNodeForPushdown(op.Cast().Left());
            bool rightResult = CheckExpressionNodeForPushdown(op.Cast().Right());
            Cout << "  Sub-branch: Checking Left and Right expressions, result: " << ((leftResult && rightResult) ? "true" : "false") << Endl;
            return leftResult && rightResult;
        }
        if (Settings.IsEnabled(EFlag::JustPassthroughOperators) && (node.Maybe<TCoCoalesce>() || node.Maybe<TCoJust>())) {
            Cout << "CheckExpressionNodeForPushdown: TCoCoalesce or TCoJust (JustPassthroughOperators enabled)" << Endl;
            for (const auto& childNodePtr : node.Ref().Children()) {
                if (!CheckExpressionNodeForPushdown(TExprBase(childNodePtr))) {
                    Cout << "  Sub-branch: Child node check failed, returning false" << Endl;
                    return false;
                }
            }
            Cout << "  Sub-branch: All child nodes passed, returning true" << Endl;
            return true;
        }
        if (const auto maybeIf = node.Maybe<TCoIf>(); maybeIf && Settings.IsEnabled(EFlag::JustPassthroughOperators)) {
            Cout << "CheckExpressionNodeForPushdown: TCoIf (JustPassthroughOperators enabled)" << Endl;
            const auto& sqlIf = maybeIf.Cast();
            const auto& predicate = sqlIf.Predicate();

            // Check if predicate pushdown
            TPredicateNode ifPredicate(predicate);
            Cout << "  Sub-branch: Checking predicate with MarkupPredicates" << Endl;
            MarkupPredicates(predicate, ifPredicate);

            // Check if expressions pushdown
            bool predicateResult = ifPredicate.CanBePushed;
            bool thenResult = CheckExpressionNodeForPushdown(sqlIf.ThenValue());
            bool elseResult = CheckExpressionNodeForPushdown(sqlIf.ElseValue());
            bool finalResult = predicateResult && thenResult && elseResult;
            
            Cout << "CheckExpressionNodeForPushdown: Predicate: " << (predicateResult ? "true" : "false")
                 << ", Then: " << (thenResult ? "true" : "false")
                 << ", Else: " << (elseResult ? "true" : "false")
                 << ", Final: " << (finalResult ? "true" : "false") << Endl;
                 
            return finalResult;
        }
        if (auto flatMap = node.Maybe<TCoFlatMap>()) {
            Cout << "CheckExpressionNodeForPushdown: TCoFlatMap" << Endl;
            return IsSupportedFlatMap(flatMap.Cast());
        }
        
        bool result = IsLambdaArgument(node);
        Cout << "CheckExpressionNodeForPushdown: Default (IsLambdaArgument), result: " << (result ? "true" : "false") << Endl;
        return result;
    }

private:
    // Comprasion checking
    bool IsSupportedLikeOperator(const TCoCompare& compare) const {
        if (!IsSimpleLikeOperator(compare)) {
            return false;
        }
        if (Settings.IsEnabled(EFlag::LikeOperator)) {
            return true;
        }
        if (Settings.IsEnabled(EFlag::LikeOperatorOnlyForUtf8) && IsUtf8Expr(compare.Left()) && IsUtf8Expr(compare.Right())) {
            return true;
        }
        return false;
    }

    bool IsSupportedCompareOperator(const TCoCompare& compare) const {
        if (compare.Maybe<TCoCmpLess>() ||
            compare.Maybe<TCoCmpLessOrEqual>() ||
            compare.Maybe<TCoCmpGreater>() ||
            compare.Maybe<TCoCmpGreaterOrEqual>() ||
            compare.Maybe<TCoCmpEqual>() ||
            compare.Maybe<TCoCmpNotEqual>() ||
            compare.Maybe<TCoAggrEqual>() ||
            compare.Maybe<TCoAggrNotEqual>()) {
            return true;
        }
        if (IsSupportedLikeOperator(compare)) {
            return true;
        }
        return false;
    }

    bool IsComparableArguments(const TExprBase& left, const TExprBase& right, bool equality) const {
        if (Settings.IsEnabled(EFlag::DoNotCheckCompareArgumentsTypes)) {
            return true;
        }

        const auto leftType = DataSlotFromOptionalDataType(left.Ref().GetTypeAnn());
        const auto rightType = DataSlotFromOptionalDataType(right.Ref().GetTypeAnn());
        if (!leftType || !rightType) {
            return IsComparableTypes(left.Ref().GetTypeAnn(), right.Ref().GetTypeAnn(), equality);
        }
        if (!Settings.IsEnabled(EFlag::StringTypes) && (IsStringType(leftType) || IsStringType(rightType))) {
            return false;
        }
        if (!Settings.IsEnabled(EFlag::DateTimeTypes) && (IsDateTimeType(leftType) || IsDateTimeType(rightType))) {
            return false;
        }
        if (!Settings.IsEnabled(EFlag::UuidType) && (IsUuidType(leftType) || IsUuidType(rightType))) {
            return false;
        }
        if (!Settings.IsEnabled(EFlag::DecimalType) && (IsDecimalType(leftType) || IsDecimalType(rightType))) {
            return false;
        }
        if (!Settings.IsEnabled(EFlag::DyNumberType) && (IsDyNumberType(leftType) || IsDyNumberType(rightType))) {
            return false;
        }
        if (leftType == rightType) {
            return true;
        }

        // We check:
        // - signed / unsigned quality by each side
        // - sizes of data types like data / interval
        switch (*leftType) {
            case NUdf::EDataSlot::Int8:
            case NUdf::EDataSlot::Int16:
            case NUdf::EDataSlot::Int32:
            case NUdf::EDataSlot::Int64:
                return Settings.IsEnabled(EFlag::ImplicitConversionToInt64) && IsSignedIntegralType(rightType);

            case NUdf::EDataSlot::Uint8:
            case NUdf::EDataSlot::Uint16:
                if (rightType == NUdf::EDataSlot::Date) {
                    return true;
                }
                [[fallthrough]];
            case NUdf::EDataSlot::Uint32:
                if (rightType == NUdf::EDataSlot::Datetime) {
                    return true;
                }
                [[fallthrough]];
            case NUdf::EDataSlot::Uint64:
                if (rightType == NUdf::EDataSlot::Timestamp || rightType == NUdf::EDataSlot::Interval) {
                    return true;
                }
                return Settings.IsEnabled(EFlag::ImplicitConversionToInt64) && IsUnsignedIntegralType(rightType);

            case NUdf::EDataSlot::Date:
                return rightType == NUdf::EDataSlot::Uint16;
            case NUdf::EDataSlot::Datetime:
                return rightType == NUdf::EDataSlot::Uint32;
            case NUdf::EDataSlot::Timestamp:
            case NUdf::EDataSlot::Interval:
                return rightType == NUdf::EDataSlot::Uint64;

            case NUdf::EDataSlot::Bool:
            case NUdf::EDataSlot::Float:
            case NUdf::EDataSlot::Double:
            case NUdf::EDataSlot::Decimal:
                return false;

            default:
                return IsComparableTypes(left.Ref().GetTypeAnn(), right.Ref().GetTypeAnn(), equality);
        }
    }

    bool IsSupportedComparisonParameters(const TCoCompare& compare) {
        const TTypeAnnotationNode* inputType = LambdaArg.Ptr()->GetTypeAnn();
        switch (inputType->GetKind()) {
            case ETypeAnnotationKind::Flow:
                inputType = inputType->Cast<TFlowExprType>()->GetItemType();
                break;
            case ETypeAnnotationKind::Stream:
                inputType = inputType->Cast<TStreamExprType>()->GetItemType();
                break;
            case ETypeAnnotationKind::Struct:
                break;
            default:
                // We do not know how process input that is not a sequence of elements
                return false;
        }
        YQL_ENSURE(inputType->GetKind() == ETypeAnnotationKind::Struct, "Unexpected predicate input type " << ui64(inputType->GetKind()));

        const auto leftList = GetComparisonNodes(compare.Left());
        const auto rightList = GetComparisonNodes(compare.Right());
        YQL_ENSURE(leftList.size() == rightList.size(), "Compression parameters should have same size but got " << leftList.size() << " vs " << rightList.size());

        for (size_t i = 0; i < leftList.size(); ++i) {
            if (!CheckExpressionNodeForPushdown(leftList[i]) || !CheckExpressionNodeForPushdown(rightList[i])) {
                return false;
            }
            if (!IsComparableArguments(leftList[i], rightList[i], compare.Maybe<TCoCmpEqual>() || compare.Maybe<TCoCmpNotEqual>())) {
                return false;
            }
        }
        return true;
    }

    bool CompareCanBePushed(const TCoCompare& compare) {
        if (!IsSupportedCompareOperator(compare)) {
            return false;
        }
        if (!IsSupportedComparisonParameters(compare)) {
            return false;
        }
        return true;
    }

private:
    // Boolean predicates checking
    bool SqlInCanBePushed(const TCoSqlIn& sqlIn) {
        if (!Settings.IsEnabled(EFlag::InOperator)) {
            return false;
        }

        const TExprBase& expr = sqlIn.Collection();
        const TExprBase& lookup = sqlIn.Lookup();
        if (!CheckExpressionNodeForPushdown(lookup)) {
            return false;
        }

        TExprNode::TPtr collection;
        if (expr.Ref().IsList()) {
            collection = expr.Ptr();
        } else if (auto maybeAsList = expr.Maybe<TCoAsList>()) {
            collection = maybeAsList.Cast().Ptr();
        } else {
            return false;
        }

        for (const auto& childNodePtr : collection->Children()) {
            if (!CheckExpressionNodeForPushdown(TExprBase(childNodePtr))) {
                return false;
            }
            if (!IsComparableArguments(lookup, TExprBase(childNodePtr), true)) {
                return false;
            }
        }
        return true;
    }

    bool IsDistinctCanBePushed(const TExprBase& predicate) {
        if (!Settings.IsEnabled(EFlag::IsDistinctOperator)) {
            return false;
        }
        if (predicate.Ref().ChildrenSize() != 2) {
            return false;
        }

        const auto left = TExprBase(predicate.Ref().Child(0));
        const auto right = TExprBase(predicate.Ref().Child(1));
        if (!CheckExpressionNodeForPushdown(left) || !CheckExpressionNodeForPushdown(right)) {
            return false;
        }
        return IsComparableArguments(left, right, true);
    }

    bool JsonExistsCanBePushed(const TCoJsonExists& jsonExists) const {
        if (!Settings.IsEnabled(EFlag::JsonExistsOperator)) {
            return false;
        }

        const auto maybeMember = jsonExists.Json().Maybe<TCoMember>();
        if (!maybeMember || !jsonExists.JsonPath().Maybe<TCoUtf8>()) {
            // Currently we support only simple columns in pushdown
            return false;
        }
        return IsMemberColumn(maybeMember.Cast());
    }

    bool CoalesceCanBePushed(const TCoCoalesce& coalesce) {
        if (!coalesce.Value().Maybe<TCoBool>()) {
            return false;
        }

        TPredicateNode predicateTree(coalesce.Predicate());
        MarkupPredicates(coalesce.Predicate(), predicateTree);
        return predicateTree.CanBePushed;
    }

    bool ExistsCanBePushed(const TCoExists& exists) const {
        return IsMemberColumn(exists.Optional());
    }

    bool UdfCanBePushed(const TCoUdf& udf, const TExprNode::TListType& children) {
        Cout << "UdfCanBePushed: " << Endl;
        const TString functionName(udf.MethodName());
        if (!Settings.IsEnabledFunction(functionName)) {
            Cout << "UdfCanBePushed: U1: " << functionName << Endl;
            return false;
        }

        if (functionName == "Re2.Grep") {
            Cout << "UdfCanBePushed: U2" << Endl;
            if (children.size() != 2) {
            Cout << "UdfCanBePushed: U3" << Endl;
                // Expected exactly one argument (first child of apply is callable)
                return false;
            }

            const auto& udfSettings = udf.Settings();
            if (udfSettings && !udfSettings.Cast().Empty()) {
            Cout << "UdfCanBePushed: U4" << Endl;
                // Expected empty udf settings
                return false;
            }

            const auto& maybeRunConfig = udf.RunConfigValue();
            if (!maybeRunConfig) {
            Cout << "UdfCanBePushed: U5" << Endl;
                // Expected non empty run config
                return false;
            }
            const auto& runConfig = maybeRunConfig.Cast().Ref();

            if (runConfig.ChildrenSize() != 2) {
            Cout << "UdfCanBePushed: U6" << Endl;
                // Expected exactly two run config settings
                return false;
            }
            if (!TExprBase(runConfig.Child(1)).Maybe<TCoNothing>()) {
            Cout << "UdfCanBePushed: U7" << Endl;
                // Expected empty regexp settings
                return false;
            }

            return CheckExpressionNodeForPushdown(TExprBase(runConfig.Child(0)));
        }

            Cout << "UdfCanBePushed: U8" << Endl;
        return false;
    }

    bool ApplyCanBePushed(const TCoApply& apply) {
        Cout << "ApplyCanBePushed: start: " << NCommon::ExprToPrettyString(Ctx, apply.Ref()) << Endl;

        TMaybeNode<TCoUdf> udf;
        Cout << "A1: " << udf.IsValid() << Endl;

        if (auto assumeStrict = apply.Raw()->Head().IsCallable("AssumeStrict") ) {
            Cout << "ApplyCanBePushed: AssumeStrict" << Endl;
            if (udf = TMaybeNode<TCoUdf>(apply.Raw()->Head().Child(0))) {
                Cout << "A2: " << udf.IsValid() << Endl;
                bool udfResult = UdfCanBePushed(udf.Cast(), apply.Raw()->Head().Child(0)->ChildrenList());
                Cout << "ApplyCanBePushed: AssumeStrict: udfResult: " << udfResult << Endl;
                if (!udfResult) {
                    return false;
                }
            };

            return false;
        }

        // Check callable
        if (udf = apply.Callable().Maybe<TCoUdf>()) {
            // Cout << "ApplyCanBePushed: Udf" << Endl;
            bool udfResult = UdfCanBePushed(udf.Cast(), apply.Ref().ChildrenList());
            if (!udfResult) {
                // Cout << "ApplyCanBePushed: Udf: false" << Endl;
                return false;
            }
        } 

        // Check arguments
        for (size_t i = 1; i < apply.Ref().ChildrenSize(); ++i) {
            Cout << "ApplyCanBePushed: Child " << i << Endl;
            if (!CheckExpressionNodeForPushdown(TExprBase(apply.Ref().Child(i)))) {
                return false;
            }
        }

        Cout << "ApplyCanBePushed: true" << Endl;
        return true;
    }

private:
    const TExprBase& LambdaArg;  // Predicate input item, has struct type
    const TSettings& Settings;
    [[maybe_unused]] TExprContext& Ctx; // To be used for pretty printing

    std::unordered_set<const TExprNode*> LambdaArguments;
};

} // anonymous namespace end

void CollectPredicates(
    TExprContext& ctx,
    const TExprBase& predicate,
    TPredicateNode& predicateTree,
    const TExprBase& lambdaArg,
    const TExprBase& /*lambdaBody*/,
    const TSettings& settings
) {
    TPredicateMarkup markup(lambdaArg, settings, ctx);
    markup.MarkupPredicates(predicate, predicateTree);
}

} // namespace NYql::NPushdown
