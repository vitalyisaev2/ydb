#pragma once

#include "yson_struct.h"

namespace NYT::NYTree {

////////////////////////////////////////////////////////////////////////////////

//! Code for mapping generation.
namespace NDetail {

template <class T, class... TArgs>
constexpr bool DifferentFrom = (!std::same_as<T, TArgs> && ...);

template <class TTuple>
struct TAllDifferentImpl;

template <>
struct TAllDifferentImpl<std::tuple<>>
{
    static constexpr bool Value = true;
};

template <class TCurrent, class... TSuffix>
struct TAllDifferentImpl<std::tuple<TCurrent, TSuffix...>>
{
    static constexpr bool Value =
        DifferentFrom<TCurrent, TSuffix...> &&
        TAllDifferentImpl<std::tuple<TSuffix...>>::Value;
};

template <class... TArgs>
constexpr bool AllDifferent = TAllDifferentImpl<std::tuple<TArgs...>>::Value;

template <class TBase, class... TArgs>
constexpr bool AllDerived = (std::derived_from<TArgs, TBase> && ...);

template <class TBase, class... TArgs>
constexpr bool CHierarchy =
    AllDerived<TBase, TArgs...> &&
    AllDifferent<TBase, TArgs...>;

////////////////////////////////////////////////////////////////////////////////

template <auto Value>
struct TFactoryTag
{ };

template <auto Value, class TDerived>
struct TLeafTag
{ };

template <class TEnum, TEnum Value, class TBase, class TDerived>
struct TMappingLeaf
{
    static TIntrusivePtr<TBase> CreateInstance();

    // This is intentionally undefined to be used in invoke_result.
    friend TDerived EnumToDerivedMethod(TFactoryTag<Value>, TMappingLeaf*);
};

template <class T, T... Value>
struct TOptionalValue;

template <class T>
struct TOptionalValue<T>
{
    static constexpr std::optional<T> OptionalValue;
};

template <class T, T Value>
struct TOptionalValue<T, Value>
{
    static constexpr std::optional<T> OptionalValue = Value;
};

template <class TEnum, class TDefaultEnumValue, class... TLeafTags>
struct TPolymorphicMapping;

template <class TEnum, TEnum... DefaultValue, TEnum BaseValue, CYsonStructDerived TBase, TEnum... Values, class... TDerived>
    requires (CHierarchy<TBase, TDerived...>)
struct TPolymorphicMapping<TEnum, TOptionalValue<TEnum, DefaultValue...>, TLeafTag<BaseValue, TBase>, TLeafTag<Values, TDerived>...>
    : public TMappingLeaf<TEnum, BaseValue, TBase, TBase>
    , public TMappingLeaf<TEnum, Values, TBase, TDerived>...
{
    template <TEnum Value, class TConcrete>
    using TLeaf = TMappingLeaf<TEnum, Value, TBase, TConcrete>;

    template <TEnum Value>
    using TDerivedToEnum = decltype(EnumToDerivedMethod(TFactoryTag<Value>{}, std::declval<TPolymorphicMapping*>()));

    using TKey = TEnum;
    using TBaseClass = TBase;

    static constexpr auto DefaultEnumValue = TOptionalValue<TEnum, DefaultValue...>::OptionalValue;

    static TIntrusivePtr<TBase> CreateInstance(TEnum value);
};

////////////////////////////////////////////////////////////////////////////////

template <class T>
constexpr bool IsMapping = requires (T t) {
    [] <class TEnum, class TDefaultEnumValue, class... TLeafTags> (TPolymorphicMapping<TEnum, TDefaultEnumValue, TLeafTags...>) {
    } (t);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

template <class TBase, class... TDerived>
concept CHierarchy = NDetail::CHierarchy<TBase, TDerived...>;

template <class TEnum, class TDefaultEnumValue, class... TLeafTags>
using TPolymorphicEnumMapping = NDetail::TPolymorphicMapping<TEnum, TDefaultEnumValue, TLeafTags...>;

template <class T>
concept CPolymorphicEnumMapping = NDetail::IsMapping<T>;

////////////////////////////////////////////////////////////////////////////////

//! Wrapper class which allows automatic (de-)serialization
//! of polymorphic types. Without it, one would have
//! to keep type serialized (e.g. in INodePtr form)
//! manually add some type marker like enum inside
//! the base object layout and then do some switches
//! over enum to properly deserialize the struct.
//! TPolymorphicYsonStruct does so almost automatically.
//! "Almost" because it still requires you to define some
//! enum marker and manually describe the hierarchy during
//! the type declaration. For most of the cases one can
//! use DEFINE_POLYMORPHIC_YSON_STRUCT to do that with
//! minimal effort. Yson layout expected/generated by this struct:
/*
    "type"   = "derived2";
    "field1" = ...;
    "field2" = ...;
*/
// NB(arkady-e1ppa): Word "type" is reserved
// and must not be used as a field name of any
// class in the hierarchy.
// TODO(arkady-e1ppa): Add customisation for reserved name.
// Would require constexpr strings and thus certain limitations.
template <CPolymorphicEnumMapping TMapping>
class TPolymorphicYsonStruct
{
    // TODO(arkady-e1ppa): Support non refcounted hierarchies
    // e.g. shared_ptr<TYsonStructLite> or shared_ptr<TExternalizedYsonStruct>.
    // TODO(arkady-e1ppa): Support lookup by enum instead of concrete type.
    // would probably need enum->Type static mapping which can be done
    // with some enum indexed tuple (But one has to implement it first)
    // TODO(arkady-e1ppa): Support ctor from anyone from the
    // hierarchy.
    using TKey = typename TMapping::TKey;
    using TBase = typename TMapping::TBaseClass;

public:
    using TImplementsYsonStructField = void;

    TPolymorphicYsonStruct() = default;

    explicit TPolymorphicYsonStruct(TKey key);
    TPolymorphicYsonStruct(TKey key, TIntrusivePtr<TBase> ptr) noexcept;

    template <CYsonStructSource TSource>
    void Load(
        TSource source,
        bool postprocess = true,
        bool setDefaults = true,
        const std::function<NYPath::TYPath()>& pathGetter = {},
        std::optional<EUnrecognizedStrategy> recursiveUnrecognizedStrategy = {});

    void Save(NYson::IYsonConsumer* consumer) const;

    //! Empty if empty or the type is wrong.
    template <std::derived_from<TBase> TConcrete>
    TIntrusivePtr<TConcrete> TryGetConcrete() const;

    template <TKey Value>
    TIntrusivePtr<typename TMapping::template TDerivedToEnum<Value>> TryGetConcrete() const;

    TKey GetCurrentType() const;

    TBase* operator->();
    const TBase* operator->() const;

    void MergeWith(const TPolymorphicYsonStruct& other);

    explicit operator bool() const;

private:
    TIntrusivePtr<TBase> Storage_;
    // TODO(arkady-e1ppa): This is a hotfix so that we don't have to bring
    // "IsSet" logic for yson structs to 24.2. Write a "better" solution for trunk
    // later.
    INodePtr SerializedStorage_;
    TKey HeldType_;

    static constexpr auto DefaultType_ = TMapping::DefaultEnumValue;

    void PrepareInstance(INodePtr& node);
};

////////////////////////////////////////////////////////////////////////////////

template <CPolymorphicEnumMapping TMapping>
void Serialize(const TPolymorphicYsonStruct<TMapping>& value, NYson::IYsonConsumer* consumer);

template <CPolymorphicEnumMapping TMapping, CYsonStructSource TSource>
void Deserialize(TPolymorphicYsonStruct<TMapping>& value, TSource source);

////////////////////////////////////////////////////////////////////////////////

//! Usage:
/*
    DEFINE_POLYMORPHIC_YSON_STRUCT(Struct,
        ((Base)     (TBaseStruct))
        ((Derived1) (TDerivedStruct1))
        ((Derived2) (TDerivedStruct2))
    );
    or
    DEFINE_POLYMORPHIC_YSON_STRUCT_WITH_DEFAULT(Struct, Derived1,
        ((Base)     (TBaseStruct))
        ((Derived1) (TDerivedStruct1))
        ((Derived2) (TDerivedStruct2))
    );

    .....TypeName.....ActualTypeName

    Macro generates two names:
    1) TStruct -- polymorphic struct which can be
    serialized.
    2) EStructType -- enum which holds short names for
    hierarchy members. They are keys in serialization.
*/
#define DEFINE_POLYMORPHIC_YSON_STRUCT(name, seq)
#define DEFINE_POLYMORPHIC_YSON_STRUCT_WITH_DEFAULT(name, default, seq)

//! Usage:
/*
    DEFINE_ENUM(EMyEnum
        (Pear)
        (Apple)
    );

    DEFINE_POLYMORPHIC_YSON_STRUCT_FOR_ENUM(Struct, EMyEnum,
        ((Pear)  (TPearClass))
        ((Apple) (TAppleClass))
    )
    or
    DEFINE_POLYMORPHIC_YSON_STRUCT_FOR_ENUM_WITH_DEFAULT(Struct, EMyEnum, Apple,
        ((Pear)  (TPearClass))
        ((Apple) (TAppleClass))
    )

    // NB(arkady-e1ppa): enum names in the list must be unqualified! E.g.

    DEFINE_POLYMORPHIC_YSON_STRUCT_FOR_ENUM(Struct, EMyEnum,
        ((EMyEnum::Pear)  (TPearClass))
        ((EMyEnum::Apple) (TAppleClass))
    )

    Will not compile
*/
#define DEFINE_POLYMORPHIC_YSON_STRUCT_FOR_ENUM(name, enum, seq)
#define DEFINE_POLYMORPHIC_YSON_STRUCT_FOR_ENUM_WITH_DEFAULT(name, enum, default, seq)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYTree

#define POLYMORPHIC_YSON_STRUCT_INL_H_
#include "polymorphic_yson_struct-inl.h"
#undef POLYMORPHIC_YSON_STRUCT_INL_H_
