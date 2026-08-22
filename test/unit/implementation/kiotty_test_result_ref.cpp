// Result<E, T&> is the reference specialisation: a pointer inside, a reference
// outside. It exists so a lookup that can fail (pool.create, table.open) can
// hand back the real object instead of a copy or a nullable pointer. The whole
// value of the type is therefore "value() is the same object", and that is an
// address comparison, not an equality one.
//
// okRef is tested separately from ok because ok decays the reference away and
// would copy - the helper exists precisely to stop that from compiling.

#include <core/kiotty_result.h>

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

using kiotty::error;
using kiotty::okRef;
using kiotty::Result;

namespace
{
    enum class LookupCode
    {
        Ok       = 0,
        NotFound = 1,
        Stale    = 2,
    };

    // Deliberately not copyable: if Result<E, T&> or okRef ever copied the
    // value instead of pointing at it, this would fail to compile.
    class Slot
    {
    public:
        explicit Slot(int id) : id(id) {}

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        int id;
    };

    typedef Result<LookupCode, Slot&> SlotResult;

    SlotResult lookupOk(Slot& slot)
    {
        return okRef(slot);
    }

    SlotResult lookupFail(LookupCode code)
    {
        return error(code);
    }

    struct FailCase
    {
        LookupCode  code;
        const char* name;
    };

    const FailCase kFailCases[] =
    {
        { LookupCode::NotFound, "NotFound" },
        { LookupCode::Stale,    "Stale" },
    };

    class RefFail : public ::testing::TestWithParam<FailCase>
    {
    };

    std::string nameOf(const ::testing::TestParamInfo<FailCase>& info)
    {
        return info.param.name;
    }
}

// The specialisation must be small and trivially copyable - it is returned by
// value from every lookup on the hot path.
static_assert(std::is_trivially_copyable<SlotResult>::value,
              "Result<E, T&> holds a pointer and a code; copying it must cost nothing");
static_assert(std::is_copy_constructible<SlotResult>::value &&
              std::is_copy_assignable<SlotResult>::value,
              "Result<E, T&> must be copyable even when T is not");

// value() on a const Result still yields T&: constness of the handle is not
// constness of the object it refers to, exactly like a pointer.
static_assert(std::is_same<decltype(std::declval<const SlotResult&>().value()), Slot&>::value,
              "const Result<E, T&>::value() must return T&, not const T&");

// -----------------------------------------------------------------------------
// success
// -----------------------------------------------------------------------------

TEST(ResultRef, OkRefProducesASuccessWithCodeZero)
{
    Slot       slot(1);
    SlotResult result = lookupOk(slot);

    EXPECT_TRUE(result.isOk());
    EXPECT_EQ(LookupCode::Ok, result.code());
}

TEST(ResultRef, OkRefValueIsTheOriginalObject)
{
    Slot       slot(1);
    SlotResult result = lookupOk(slot);

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(&slot, &result.value());
}

TEST(ResultRef, MutationThroughValueReachesTheOriginal)
{
    Slot       slot(1);
    SlotResult result = lookupOk(slot);

    result.value().id = 99;
    EXPECT_EQ(99, slot.id);
}

TEST(ResultRef, ConstResultValueStillRefersToTheOriginal)
{
    Slot             slot(1);
    const SlotResult result = lookupOk(slot);

    EXPECT_EQ(&slot, &result.value());

    result.value().id = 7;
    EXPECT_EQ(7, slot.id);
}

// The primary template reads operator bool as "has an error", and the
// specialisation must agree or a caller switching between the two would
// invert every branch.
TEST(ResultRef, OperatorBoolIsFalseOnSuccessLikeThePrimaryTemplate)
{
    Slot       slot(1);
    SlotResult result = lookupOk(slot);

    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_TRUE(result.isOk());
}

// -----------------------------------------------------------------------------
// failure
// -----------------------------------------------------------------------------

TEST_P(RefFail, ErrorProducesAFailureThatKeepsItsCode)
{
    SlotResult result = lookupFail(GetParam().code);

    EXPECT_FALSE(result.isOk());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(GetParam().code, result.code());
}

INSTANTIATE_TEST_SUITE_P(AllCodes, RefFail, ::testing::ValuesIn(kFailCases), nameOf);

// -----------------------------------------------------------------------------
// copying
// -----------------------------------------------------------------------------

TEST(ResultRef, CopyConstructedResultRefersToTheSameObject)
{
    Slot       slot(1);
    SlotResult original = lookupOk(slot);
    SlotResult copy(original);

    EXPECT_TRUE(copy.isOk());
    EXPECT_EQ(&slot, &copy.value());
    EXPECT_EQ(&original.value(), &copy.value());
}

TEST(ResultRef, CopyAssignedResultRefersToTheSameObject)
{
    Slot       first(1);
    Slot       second(2);
    SlotResult target = lookupOk(first);
    SlotResult source = lookupOk(second);

    target = source;

    EXPECT_EQ(&second, &target.value());
}

TEST(ResultRef, CopyAssigningAFailureOverASuccessKeepsTheFailureCode)
{
    Slot       slot(1);
    SlotResult target = lookupOk(slot);
    SlotResult source = lookupFail(LookupCode::Stale);

    target = source;

    EXPECT_FALSE(target.isOk());
    EXPECT_EQ(LookupCode::Stale, target.code());
}

TEST(ResultRef, CopyAssigningASuccessOverAFailureRestoresTheObject)
{
    Slot       slot(1);
    SlotResult target = lookupOk(slot);
    SlotResult failed = lookupFail(LookupCode::NotFound);

    target = failed;
    ASSERT_FALSE(target.isOk());

    target = lookupOk(slot);
    EXPECT_TRUE(target.isOk());
    EXPECT_EQ(&slot, &target.value());
}

TEST(ResultRef, CopiedFailureKeepsItsCode)
{
    SlotResult original = lookupFail(LookupCode::NotFound);
    SlotResult copy(original);

    EXPECT_FALSE(copy.isOk());
    EXPECT_EQ(LookupCode::NotFound, copy.code());
}

// -----------------------------------------------------------------------------
// precondition
// -----------------------------------------------------------------------------

// assert() is what guards value() on a failed result, so this can only be
// observed in a build that keeps asserts. MSVC's debug assert opens a dialog
// box instead of aborting, which a death test cannot see through, so the
// check is Linux-only as well.
#if !defined(NDEBUG) && !defined(_WIN32) && GTEST_HAS_DEATH_TEST
TEST(ResultRefDeath, ValueOnAFailedResultAsserts)
{
    SlotResult result = lookupFail(LookupCode::NotFound);

    EXPECT_DEATH(result.value(), "value\\(\\) on a failed result");
}
#endif
