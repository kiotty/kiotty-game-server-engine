// Result keeps its value in a union, which buys the failure path the right to
// construct nothing at all - and costs the type the compiler's help with
// lifetimes. Every copy, move and assignment has to start and end that union
// member by hand, and getting one of the four assignment combinations wrong
// leaks or double-destroys without any other symptom.
//
// So the value type here is one that counts, and the assignment combinations
// are a table rather than four hand-written tests.

#include <core/kiotty_result.h>

#include "support/kiotty_test_tracked.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

using kiotty::Result;
using kiotty_test::Ledger;
using kiotty_test::Tracked;

namespace
{
    // Zero has to mean success - Result reads code() == 0 as ok.
    enum class SendCode
    {
        Ok        = 0,
        Refused   = 1,
        Exhausted = 2,
    };

    typedef Result<SendCode, Tracked> TrackedResult;

    TrackedResult okWith(Ledger& ledger, int id)
    {
        return kiotty::ok(Tracked(ledger, id));
    }

    TrackedResult failWith(SendCode code)
    {
        return kiotty::error(code);
    }

    struct AssignCase
    {
        bool        target_ok;
        bool        source_ok;
        bool        by_move;
        const char* name;     // identifier-safe: this is what ctest lists
        const char* label;
    };

    // All four combinations, each done both ways. ok <- fail is the one that
    // has to destroy a value, fail <- ok is the one that has to start one, and
    // the two matching pairs are where an implementation that always destroys
    // or always constructs shows up.
    const AssignCase kAssignCases[] =
    {
        { true,  true,  false, "CopyOkFromOk",     "copy: ok <- ok" },
        { true,  false, false, "CopyOkFromFail",   "copy: ok <- fail" },
        { false, true,  false, "CopyFailFromOk",   "copy: fail <- ok" },
        { false, false, false, "CopyFailFromFail", "copy: fail <- fail" },
        { true,  true,  true,  "MoveOkFromOk",     "move: ok <- ok" },
        { true,  false, true,  "MoveOkFromFail",   "move: ok <- fail" },
        { false, true,  true,  "MoveFailFromOk",   "move: fail <- ok" },
        { false, false, true,  "MoveFailFromFail", "move: fail <- fail" },
    };

    class Assign : public ::testing::TestWithParam<AssignCase>
    {
    };

    // Without this gtest names each row by dumping the struct as bytes, and
    // ctest -R becomes unusable on exactly the tests that need it most.
    std::string nameOf(const ::testing::TestParamInfo<AssignCase>& info)
    {
        return info.param.name;
    }
}

TEST(Result, OkResultReportsSuccessAndCarriesTheValue)
{
    Ledger ledger;

    {
        const TrackedResult result = okWith(ledger, 5);

        EXPECT_TRUE(result.isOk());
        EXPECT_EQ(SendCode::Ok, result.code());
        EXPECT_EQ(5, result.value().id());
        EXPECT_EQ(1, ledger.live());
    }

    EXPECT_EQ(0, ledger.live());
}

TEST(Result, FailedResultCarriesTheCodeAndConstructsNoValue)
{
    Ledger ledger;

    {
        const TrackedResult result = failWith(SendCode::Refused);

        EXPECT_FALSE(result.isOk());
        EXPECT_EQ(SendCode::Refused, result.code());

        // Not "constructed and then ignored" - never constructed. That is what
        // the union is for, and a Value with a real cost would pay it here.
        EXPECT_EQ(0, ledger.acquired);
    }

    EXPECT_EQ(0, ledger.live());
}

TEST(Result, OperatorBoolReportsFailureRatherThanSuccess)
{
    Ledger ledger;

    const TrackedResult good = okWith(ledger, 6);
    const TrackedResult bad  = failWith(SendCode::Exhausted);

    // NOTE: this reads backwards from every other operator bool in the engine,
    // where true means "there is something here". Result says true when there
    // is an error code. It is pinned here so that changing it is a decision
    // someone makes rather than something that happens.
    EXPECT_FALSE(static_cast<bool>(good));
    EXPECT_TRUE(static_cast<bool>(bad));

    EXPECT_TRUE(good.isOk());
    EXPECT_FALSE(bad.isOk());
}

TEST(Result, CopyingAnOkResultProducesAnIndependentValue)
{
    Ledger ledger;

    {
        const TrackedResult source = okWith(ledger, 7);
        const int           before = ledger.copies;

        const TrackedResult copy(source);

        EXPECT_TRUE(copy.isOk());
        EXPECT_EQ(7, copy.value().id());
        EXPECT_EQ(before + 1, ledger.copies);

        // Both are alive; a copy that shared the source's value would show one.
        EXPECT_EQ(2, ledger.live());
    }

    EXPECT_EQ(0, ledger.live());
}

TEST(Result, CopyingAFailedResultStartsNoValue)
{
    Ledger ledger;

    {
        const TrackedResult source = failWith(SendCode::Refused);
        const TrackedResult copy(source);

        EXPECT_FALSE(copy.isOk());
        EXPECT_EQ(SendCode::Refused, copy.code());
        EXPECT_EQ(0, ledger.acquired);
    }

    EXPECT_EQ(0, ledger.live());
}

TEST(Result, MovingAnOkResultMovesTheValueRatherThanCopyingIt)
{
    Ledger ledger;

    {
        TrackedResult source = okWith(ledger, 8);

        const int copies_before = ledger.copies;

        const TrackedResult moved(std::move(source));

        EXPECT_TRUE(moved.isOk());
        EXPECT_EQ(8, moved.value().id());
        EXPECT_EQ(copies_before, ledger.copies) << "the move copied instead";

        // The source Result is still ok() and still owns a Tracked - a moved-from
        // Result is not an empty one, it is one whose value has been moved from.
        EXPECT_TRUE(source.isOk());
        EXPECT_EQ(1, ledger.live());
    }

    EXPECT_EQ(0, ledger.live());
}

TEST(Result, MovingAFailedResultStartsNoValue)
{
    Ledger ledger;

    {
        TrackedResult       source = failWith(SendCode::Exhausted);
        const TrackedResult moved(std::move(source));

        EXPECT_FALSE(moved.isOk());
        EXPECT_EQ(SendCode::Exhausted, moved.code());
        EXPECT_EQ(0, ledger.acquired);
    }

    EXPECT_EQ(0, ledger.live());
}

TEST_P(Assign, LeavesNoValueBehindWhicheverWayItGoes)
{
    const AssignCase& sample = GetParam();

    SCOPED_TRACE(sample.label);

    Ledger ledger;

    {
        TrackedResult target = sample.target_ok ? okWith(ledger, 1)
                                                : failWith(SendCode::Refused);
        TrackedResult source = sample.source_ok ? okWith(ledger, 2)
                                                : failWith(SendCode::Exhausted);

        if (sample.by_move)
        {
            target = std::move(source);
        }
        else
        {
            target = source;
        }

        EXPECT_EQ(sample.source_ok, target.isOk());

        if (sample.source_ok)
        {
            EXPECT_EQ(2, target.value().id());
        }
        else
        {
            EXPECT_EQ(SendCode::Exhausted, target.code());
        }
    }

    // The ledger is the whole assertion: anything above zero is a value that
    // was overwritten without being destroyed, anything below is one destroyed
    // twice.
    EXPECT_EQ(0, ledger.live());
}

INSTANTIATE_TEST_SUITE_P(EveryAssignmentCombination, Assign,
                         ::testing::ValuesIn(kAssignCases), nameOf);

TEST(Result, SelfAssignmentKeepsTheValue)
{
    Ledger ledger;

    {
        TrackedResult        result = okWith(ledger, 9);
        const TrackedResult& alias  = result;

        result = alias;

        EXPECT_TRUE(result.isOk());
        EXPECT_EQ(9, result.value().id());
        EXPECT_EQ(1, ledger.live());
    }

    EXPECT_EQ(0, ledger.live());
}

TEST(Result, WorksWithAPlainIntegralErrorCode)
{
    // ErrorCode is allowed to be an integral type as well as an enum, and the
    // socket layer uses both. Zero still has to mean success.
    const Result<int, int> good = kiotty::ok(42);
    const Result<int, int> bad  = kiotty::error(7);

    EXPECT_TRUE(good.isOk());
    EXPECT_EQ(42, good.value());
    EXPECT_FALSE(bad.isOk());
    EXPECT_EQ(7, bad.code());
}
