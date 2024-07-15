# Generated by devtools/yamaker.

LIBRARY()

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/clang14
    contrib/libs/clang14/include
    contrib/libs/clang14/lib
    contrib/libs/clang14/lib/AST
    contrib/libs/clang14/lib/ASTMatchers
    contrib/libs/clang14/lib/Analysis
    contrib/libs/clang14/lib/Basic
    contrib/libs/clang14/lib/Lex
    contrib/libs/clang14/lib/Tooling
    contrib/libs/clang14/tools/extra/clang-tidy/utils
    contrib/libs/llvm14
    contrib/libs/llvm14/lib/Frontend/OpenMP
    contrib/libs/llvm14/lib/Support
)

ADDINCL(
    contrib/libs/clang14/tools/extra/clang-tidy/readability
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    AvoidConstParamsInDecls.cpp
    BracesAroundStatementsCheck.cpp
    ConstReturnTypeCheck.cpp
    ContainerContainsCheck.cpp
    ContainerDataPointerCheck.cpp
    ContainerSizeEmptyCheck.cpp
    ConvertMemberFunctionsToStatic.cpp
    DeleteNullPointerCheck.cpp
    DuplicateIncludeCheck.cpp
    ElseAfterReturnCheck.cpp
    FunctionCognitiveComplexityCheck.cpp
    FunctionSizeCheck.cpp
    IdentifierLengthCheck.cpp
    IdentifierNamingCheck.cpp
    ImplicitBoolConversionCheck.cpp
    InconsistentDeclarationParameterNameCheck.cpp
    IsolateDeclarationCheck.cpp
    MagicNumbersCheck.cpp
    MakeMemberFunctionConstCheck.cpp
    MisleadingIndentationCheck.cpp
    MisplacedArrayIndexCheck.cpp
    NamedParameterCheck.cpp
    NamespaceCommentCheck.cpp
    NonConstParameterCheck.cpp
    QualifiedAutoCheck.cpp
    ReadabilityTidyModule.cpp
    RedundantAccessSpecifiersCheck.cpp
    RedundantControlFlowCheck.cpp
    RedundantDeclarationCheck.cpp
    RedundantFunctionPtrDereferenceCheck.cpp
    RedundantMemberInitCheck.cpp
    RedundantPreprocessorCheck.cpp
    RedundantSmartptrGetCheck.cpp
    RedundantStringCStrCheck.cpp
    RedundantStringInitCheck.cpp
    SimplifyBooleanExprCheck.cpp
    SimplifySubscriptExprCheck.cpp
    StaticAccessedThroughInstanceCheck.cpp
    StaticDefinitionInAnonymousNamespaceCheck.cpp
    StringCompareCheck.cpp
    SuspiciousCallArgumentCheck.cpp
    UniqueptrDeleteReleaseCheck.cpp
    UppercaseLiteralSuffixCheck.cpp
    UseAnyOfAllOfCheck.cpp
)

END()