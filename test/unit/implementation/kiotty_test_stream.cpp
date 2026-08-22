// MutableStream is a fixed array of non-owning listener pointers and nothing
// else - no heap, no copy of the item, no deferred delivery. What makes it worth
// testing is the handful of promises the rest of the engine builds on: every
// listener hears every item in registration order, the same object is handed
// to each of them, and the capacity bound is honest in both directions.
//
// The one behaviour that is pinned rather than promised is removal during
// emit. The implementation shifts the array under the running loop, so a
// listener that removes itself from inside onStream makes the loop skip its
// successor. That test documents what happens today; it is not a claim that
// this is the right behaviour.

#include <core/kiotty_stream.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

using kiotty::ISink;
using kiotty::IStream;
using kiotty::MutableStream;
using kiotty::StreamListener;

namespace
{
    // Every listener writes its tag into one shared log, so delivery order is a
    // single vector compare rather than a per-listener call count.
    class TaggedListener : public StreamListener<int>
    {
    public:
        TaggedListener(std::vector<int>& log, int tag) :
            calls(0),
            last_item(nullptr),
            _log(log),
            _tag(tag)
        {
        }

        void onStream(const int& item) override
        {
            ++calls;
            last_item = &item;
            _log.push_back(_tag);
        }

        int        calls;
        const int* last_item;

    private:
        std::vector<int>& _log;
        int               _tag;
    };

    // Removes itself from the stream it was given the first time it is called.
    // This is the reentrancy case a Connection that closes in response to an
    // item would produce.
    class SelfRemovingListener : public StreamListener<int>
    {
    public:
        SelfRemovingListener(std::vector<int>& log, int tag, IStream<int>& owner) :
            _log(log),
            _tag(tag),
            _owner(owner)
        {
        }

        void onStream(const int&) override
        {
            _log.push_back(_tag);
            _owner.removeListener(*this);
        }

    private:
        std::vector<int>& _log;
        int               _tag;
        IStream<int>&     _owner;
    };

    // The capacity bound is a template parameter, so the boundary has to be
    // walked once per instantiation. 1 is the smallest the static_assert
    // allows, 4 is the default every GameChannel stream uses.
    template <typename Capacity>
    class StreamCapacity : public ::testing::Test
    {
    };

    typedef ::testing::Types<
        std::integral_constant<size_t, 1>,
        std::integral_constant<size_t, 2>,
        std::integral_constant<size_t, 4> > kCapacities;

    TYPED_TEST_SUITE(StreamCapacity, kCapacities);
}

// -----------------------------------------------------------------------------
// emit
// -----------------------------------------------------------------------------

TEST(MutableStream, EmitWithNoListenerReturnsFalse)
{
    MutableStream<int> stream;

    EXPECT_FALSE(stream.emit(1));
    EXPECT_EQ(0u, stream.listenerCount());
}

TEST(MutableStream, EmitDeliversToEveryListenerInRegistrationOrder)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);
    TaggedListener     b(log, 2);
    TaggedListener     c(log, 3);

    ASSERT_TRUE(stream.addListener(a));
    ASSERT_TRUE(stream.addListener(b));
    ASSERT_TRUE(stream.addListener(c));

    EXPECT_TRUE(stream.emit(7));

    const std::vector<int> expected = { 1, 2, 3 };
    EXPECT_EQ(expected, log);
}

TEST(MutableStream, EmitHandsTheSameObjectToEveryListener)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);
    TaggedListener     b(log, 2);

    ASSERT_TRUE(stream.addListener(a));
    ASSERT_TRUE(stream.addListener(b));

    // The item must reach the listeners by reference: a move-only payload
    // (GameResponse) would be unusable by the second listener otherwise.
    const int item = 42;
    EXPECT_TRUE(stream.emit(item));
    EXPECT_EQ(&item, a.last_item);
    EXPECT_EQ(&item, b.last_item);
}

TEST(MutableStream, EmitTwiceDeliversTwiceToEachListener)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);

    ASSERT_TRUE(stream.addListener(a));
    EXPECT_TRUE(stream.emit(1));
    EXPECT_TRUE(stream.emit(2));
    EXPECT_EQ(2, a.calls);
}

// -----------------------------------------------------------------------------
// addListener
// -----------------------------------------------------------------------------

TYPED_TEST(StreamCapacity, AddListenerSucceedsUpToCapacityAndFailsPastIt)
{
    const size_t capacity = TypeParam::value;

    std::vector<int>                 log;
    MutableStream<int, TypeParam::value> stream;
    std::vector<TaggedListener>      listeners;

    listeners.reserve(capacity + 1);
    for (size_t i = 0; i < capacity + 1; ++i)
    {
        listeners.push_back(TaggedListener(log, static_cast<int>(i)));
    }

    for (size_t i = 0; i < capacity; ++i)
    {
        SCOPED_TRACE(i);
        EXPECT_TRUE(stream.addListener(listeners[i]));
        EXPECT_EQ(i + 1, stream.listenerCount());
    }

    EXPECT_FALSE(stream.addListener(listeners[capacity]));
    EXPECT_EQ(capacity, stream.listenerCount());

    // The refused listener must not be called; the accepted ones all must.
    EXPECT_TRUE(stream.emit(0));
    EXPECT_EQ(capacity, log.size());
    EXPECT_EQ(0, listeners[capacity].calls);
}

TYPED_TEST(StreamCapacity, RemoveListenerOnFullStreamFreesOneSlot)
{
    const size_t capacity = TypeParam::value;

    std::vector<int>                 log;
    MutableStream<int, TypeParam::value> stream;
    std::vector<TaggedListener>      listeners;

    listeners.reserve(capacity + 1);
    for (size_t i = 0; i < capacity + 1; ++i)
    {
        listeners.push_back(TaggedListener(log, static_cast<int>(i)));
    }
    for (size_t i = 0; i < capacity; ++i)
    {
        ASSERT_TRUE(stream.addListener(listeners[i]));
    }
    ASSERT_FALSE(stream.addListener(listeners[capacity]));

    stream.removeListener(listeners[0]);
    EXPECT_EQ(capacity - 1, stream.listenerCount());
    EXPECT_TRUE(stream.addListener(listeners[capacity]));
    EXPECT_EQ(capacity, stream.listenerCount());
}

TEST(MutableStream, AddListenerTwiceReturnsFalseAndKeepsSingleRegistration)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);

    ASSERT_TRUE(stream.addListener(a));
    EXPECT_FALSE(stream.addListener(a));
    EXPECT_EQ(1u, stream.listenerCount());

    // A duplicate that had slipped in would show up as a double delivery.
    EXPECT_TRUE(stream.emit(0));
    EXPECT_EQ(1, a.calls);
}

TEST(MutableStream, AddListenerAfterRemovalAppendsAtTheEnd)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);
    TaggedListener     b(log, 2);
    TaggedListener     c(log, 3);

    ASSERT_TRUE(stream.addListener(a));
    ASSERT_TRUE(stream.addListener(b));
    ASSERT_TRUE(stream.addListener(c));

    stream.removeListener(a);
    ASSERT_TRUE(stream.addListener(a));

    EXPECT_TRUE(stream.emit(0));

    const std::vector<int> expected = { 2, 3, 1 };
    EXPECT_EQ(expected, log);
}

// -----------------------------------------------------------------------------
// removeListener
// -----------------------------------------------------------------------------

TEST(MutableStream, RemoveListenerOfUnregisteredListenerIsIgnored)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);
    TaggedListener     stranger(log, 9);

    ASSERT_TRUE(stream.addListener(a));

    stream.removeListener(stranger);
    EXPECT_EQ(1u, stream.listenerCount());

    EXPECT_TRUE(stream.emit(0));
    EXPECT_EQ(1, a.calls);
}

TEST(MutableStream, RemoveListenerOnEmptyStreamIsIgnored)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);

    stream.removeListener(a);
    EXPECT_EQ(0u, stream.listenerCount());
    EXPECT_FALSE(stream.emit(0));
}

TEST(MutableStream, RemoveMiddleListenerKeepsOrderOfTheOthers)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);
    TaggedListener     b(log, 2);
    TaggedListener     c(log, 3);

    ASSERT_TRUE(stream.addListener(a));
    ASSERT_TRUE(stream.addListener(b));
    ASSERT_TRUE(stream.addListener(c));

    stream.removeListener(b);
    EXPECT_EQ(2u, stream.listenerCount());

    EXPECT_TRUE(stream.emit(0));

    const std::vector<int> expected = { 1, 3 };
    EXPECT_EQ(expected, log);
    EXPECT_EQ(0, b.calls);
}

TEST(MutableStream, RemoveLastListenerMakesEmitReturnFalse)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);

    ASSERT_TRUE(stream.addListener(a));
    stream.removeListener(a);

    EXPECT_EQ(0u, stream.listenerCount());
    EXPECT_FALSE(stream.emit(0));
    EXPECT_EQ(0, a.calls);
}

TEST(MutableStream, RemoveListenerTwiceIsIdempotent)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);
    TaggedListener     b(log, 2);

    ASSERT_TRUE(stream.addListener(a));
    ASSERT_TRUE(stream.addListener(b));

    stream.removeListener(a);
    stream.removeListener(a);

    EXPECT_EQ(1u, stream.listenerCount());
    EXPECT_TRUE(stream.emit(0));
    EXPECT_EQ(1, b.calls);
}

// Pinned behaviour, not a contract. The loop in emit reads _count live and the
// array is shifted left on removal, so when listener A removes itself at
// index 0, B moves to index 0 while the loop advances to index 1 and hits C.
// B is never called for this emit. If this test starts failing, the
// implementation changed its reentrancy semantics - update the expectation
// deliberately rather than by accident.
TEST(MutableStream, SelfRemovalInsideOnStreamSkipsTheNextListener)
{
    std::vector<int>     log;
    MutableStream<int>   stream;
    SelfRemovingListener a(log, 1, stream.stream());
    TaggedListener       b(log, 2);
    TaggedListener       c(log, 3);

    ASSERT_TRUE(stream.addListener(a));
    ASSERT_TRUE(stream.addListener(b));
    ASSERT_TRUE(stream.addListener(c));

    EXPECT_TRUE(stream.emit(0));

    const std::vector<int> observed = { 1, 3 };
    EXPECT_EQ(observed, log);
    EXPECT_EQ(0, b.calls);
    EXPECT_EQ(1, c.calls);
    EXPECT_EQ(2u, stream.listenerCount());

    // Once the reentrant emit is over the stream is consistent again: the
    // next emit reaches both survivors in order.
    log.clear();
    EXPECT_TRUE(stream.emit(0));

    const std::vector<int> after = { 2, 3 };
    EXPECT_EQ(after, log);
}

TEST(MutableStream, SelfRemovalOfTheLastListenerLosesNothing)
{
    std::vector<int>     log;
    MutableStream<int>   stream;
    TaggedListener       a(log, 1);
    SelfRemovingListener b(log, 2, stream.stream());

    ASSERT_TRUE(stream.addListener(a));
    ASSERT_TRUE(stream.addListener(b));

    EXPECT_TRUE(stream.emit(0));

    const std::vector<int> expected = { 1, 2 };
    EXPECT_EQ(expected, log);
    EXPECT_EQ(1u, stream.listenerCount());
}

// -----------------------------------------------------------------------------
// clear
// -----------------------------------------------------------------------------

TEST(MutableStream, ClearThenEmitReturnsFalseAndCallsNobody)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);
    TaggedListener     b(log, 2);

    ASSERT_TRUE(stream.addListener(a));
    ASSERT_TRUE(stream.addListener(b));

    stream.clear();

    EXPECT_EQ(0u, stream.listenerCount());
    EXPECT_FALSE(stream.emit(0));
    EXPECT_EQ(0, a.calls);
    EXPECT_EQ(0, b.calls);
}

TEST(MutableStream, ClearThenAddListenerAcceptsTheFullCapacityAgain)
{
    std::vector<int>      log;
    MutableStream<int, 2> stream;
    TaggedListener        a(log, 1);
    TaggedListener        b(log, 2);

    ASSERT_TRUE(stream.addListener(a));
    ASSERT_TRUE(stream.addListener(b));
    stream.clear();

    // Re-adding the same objects must work: clear really forgot them, it did
    // not just stop calling them.
    EXPECT_TRUE(stream.addListener(a));
    EXPECT_TRUE(stream.addListener(b));
    EXPECT_EQ(2u, stream.listenerCount());
}

TEST(MutableStream, ClearOnEmptyStreamIsHarmless)
{
    MutableStream<int> stream;

    stream.clear();
    EXPECT_EQ(0u, stream.listenerCount());
    EXPECT_FALSE(stream.emit(0));
}

// -----------------------------------------------------------------------------
// views
// -----------------------------------------------------------------------------

TEST(MutableStream, SinkAndStreamViewsAreTheSameObject)
{
    MutableStream<int> stream;

    ISink<int>&   sink = stream.sink();
    IStream<int>& view = stream.stream();

    EXPECT_EQ(static_cast<ISink<int>*>(&stream), &sink);
    EXPECT_EQ(static_cast<IStream<int>*>(&stream), &view);
}

TEST(MutableStream, ListenerAddedThroughStreamViewHearsEmitThroughSinkView)
{
    std::vector<int>   log;
    MutableStream<int> stream;
    TaggedListener     a(log, 1);

    ASSERT_TRUE(stream.stream().addListener(a));
    EXPECT_TRUE(stream.sink().emit(5));
    EXPECT_EQ(1, a.calls);
}
