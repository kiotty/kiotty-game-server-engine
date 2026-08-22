// Holder exists to get polymorphism without a heap allocation, and the price of
// that is that it has to run the destructor itself. So the only question worth
// asking is how many times it runs: once per object, never twice, never zero.
//
// A moved-from Holder is where both failures live. If it still points at the
// storage it gave away, the destructor runs on an object that has moved; if the
// new one forgets the lifecycle table, it never runs at all. Neither shows up
// as a crash on a type made of ints, which is why the held object owns
// something the ledger can count.

#include <core/kiotty_holder.h>

#include "support/kiotty_test_tracked.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <utility>

using kiotty::Holder;
using kiotty_test::Ledger;
using kiotty_test::Tracked;

namespace
{
    class Shape
    {
    public:
        // Holder sizes its buffer from these, so they belong to the base rather
        // than to any one derived type.
        static const size_t HOLDER_SIZE  = 64;
        static const size_t HOLDER_ALIGN = 8;

        virtual ~Shape() {}

        virtual int sides() const = 0;
    };

    class Triangle : public Shape
    {
    public:
        Triangle(Ledger& ledger, int id) :
            _resource(ledger, id)
        {
        }

        // Shape has a user-declared destructor, so nothing here is implicit.
        // Holder refuses a type whose move can throw, so it is said out loud.
        Triangle(Triangle&& other) noexcept :
            Shape(other),
            _resource(std::move(other._resource))
        {
        }

        int sides() const override { return 3; }

        int id() const { return _resource.id(); }

    private:
        Tracked _resource;
    };

    class Square : public Shape
    {
    public:
        Square(Ledger& ledger, int id) :
            _resource(ledger, id)
        {
        }

        Square(Square&& other) noexcept :
            Shape(other),
            _resource(std::move(other._resource))
        {
        }

        int sides() const override { return 4; }

        int id() const { return _resource.id(); }

    private:
        Tracked _resource;
    };

    // Self-move written directly is diagnosed, and the warning is right about
    // ordinary code. Going through a function lets the test ask what the
    // operator actually does with it.
    void moveOnto(Holder<Shape>& target, Holder<Shape>& source)
    {
        target = std::move(source);
    }
}

TEST(Holder, DefaultHolderIsFalseAndHoldsNothing)
{
    Holder<Shape> holder;

    EXPECT_FALSE(static_cast<bool>(holder));
}

TEST(Holder, MakeConstructsTheDerivedAndReachesItThroughTheBase)
{
    Ledger ledger;

    Holder<Shape> holder = Holder<Shape>::make<Triangle>(ledger, 11);

    ASSERT_TRUE(static_cast<bool>(holder));
    EXPECT_EQ(3, holder->sides());
    EXPECT_EQ(3, (*holder).sides());
    EXPECT_EQ(1, ledger.live());
}

TEST(Holder, DestructionRunsTheDerivedDestructorExactlyOnce)
{
    Ledger ledger;

    {
        Holder<Shape> holder = Holder<Shape>::make<Square>(ledger, 12);
        ASSERT_EQ(1, ledger.live());
    }

    EXPECT_EQ(0, ledger.live()) << "the held object was not destroyed";
    EXPECT_EQ(1, ledger.destructor_runs) << "it was destroyed more than once";
}

TEST(Holder, MoveConstructionLeavesTheSourceEmptyAndKeepsOneLiveObject)
{
    Ledger ledger;

    {
        Holder<Shape> source = Holder<Shape>::make<Triangle>(ledger, 13);
        Holder<Shape> moved(std::move(source));

        EXPECT_TRUE(static_cast<bool>(moved));
        EXPECT_EQ(3, moved->sides());

        // The source must be empty, not merely stale: an empty Holder is the
        // only state whose destructor does nothing.
        EXPECT_FALSE(static_cast<bool>(source));

        // The object was relocated, not duplicated.
        EXPECT_EQ(1, ledger.live());
    }

    EXPECT_EQ(0, ledger.live());
    EXPECT_EQ(1, ledger.destructor_runs);
}

TEST(Holder, MoveConstructionRecomputesTheBasePointerForTheNewStorage)
{
    Ledger ledger;

    Holder<Shape> source = Holder<Shape>::make<Square>(ledger, 14);
    Shape* const  before = &(*source);

    Holder<Shape> moved(std::move(source));

    // The object lives inside the Holder, so moving the Holder moves the
    // object. A base pointer copied across rather than recomputed would still
    // point into the storage of the Holder that no longer owns anything.
    EXPECT_NE(before, &(*moved));
    EXPECT_EQ(4, moved->sides());
}

TEST(Holder, MoveAssignmentDestroysWhatTheTargetWasHolding)
{
    Ledger ledger;

    {
        Holder<Shape> target = Holder<Shape>::make<Triangle>(ledger, 15);
        Holder<Shape> source = Holder<Shape>::make<Square>(ledger, 16);

        ASSERT_EQ(2, ledger.live());

        target = std::move(source);

        // The triangle went; the square moved across. Two live objects here
        // would mean the triangle was overwritten without being destroyed.
        EXPECT_EQ(1, ledger.live());
        EXPECT_EQ(4, target->sides());
        EXPECT_FALSE(static_cast<bool>(source));
    }

    EXPECT_EQ(0, ledger.live());
}

TEST(Holder, MoveAssignmentOntoAnEmptyHolderJustTakesTheObject)
{
    Ledger ledger;

    {
        Holder<Shape> target;
        Holder<Shape> source = Holder<Shape>::make<Square>(ledger, 17);

        target = std::move(source);

        EXPECT_TRUE(static_cast<bool>(target));
        EXPECT_EQ(1, ledger.live());
    }

    EXPECT_EQ(0, ledger.live());
}

TEST(Holder, MoveAssignmentFromAnEmptyHolderEmptiesTheTarget)
{
    Ledger ledger;

    Holder<Shape> target = Holder<Shape>::make<Triangle>(ledger, 18);
    Holder<Shape> source;

    target = std::move(source);

    // moveFrom returns early on an empty source, but reset() already ran, so
    // the target must be empty rather than still holding the triangle.
    EXPECT_FALSE(static_cast<bool>(target));
    EXPECT_EQ(0, ledger.live());
}

TEST(Holder, SelfMoveAssignmentKeepsTheObject)
{
    Ledger ledger;

    Holder<Shape> holder = Holder<Shape>::make<Square>(ledger, 19);

    moveOnto(holder, holder);

    EXPECT_TRUE(static_cast<bool>(holder));
    EXPECT_EQ(4, holder->sides());
    EXPECT_EQ(1, ledger.live());
}

TEST(Holder, ResetDestroysTheObjectAndIsIdempotent)
{
    Ledger ledger;

    Holder<Shape> holder = Holder<Shape>::make<Triangle>(ledger, 20);

    holder.reset();

    EXPECT_FALSE(static_cast<bool>(holder));
    EXPECT_EQ(0, ledger.live());
    EXPECT_EQ(1, ledger.destructor_runs);

    // A second reset must not run the destructor again on storage whose object
    // is already gone.
    holder.reset();

    EXPECT_FALSE(static_cast<bool>(holder));
    EXPECT_EQ(1, ledger.destructor_runs);
}

TEST(Holder, ResetOnADefaultHolderDoesNothing)
{
    Ledger        ledger;
    Holder<Shape> holder;

    holder.reset();

    EXPECT_FALSE(static_cast<bool>(holder));
    EXPECT_EQ(0, ledger.destructor_runs);
}

TEST(Holder, DifferentDerivedTypesEachGetTheirOwnDestructor)
{
    Ledger ledger;

    {
        Holder<Shape> triangle = Holder<Shape>::make<Triangle>(ledger, 21);
        Holder<Shape> square   = Holder<Shape>::make<Square>(ledger, 22);

        EXPECT_EQ(3, triangle->sides());
        EXPECT_EQ(4, square->sides());
        EXPECT_EQ(2, ledger.live());
    }

    EXPECT_EQ(0, ledger.live());
    EXPECT_EQ(2, ledger.destructor_runs);
}
