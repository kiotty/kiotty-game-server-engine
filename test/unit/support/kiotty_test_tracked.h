#if !defined(KIOTTY_TEST_TRACKED_H)
#define KIOTTY_TEST_TRACKED_H

namespace kiotty_test
{
    // Where the Tracked objects of one test report what happened to them.
    //
    // The counters are here rather than in statics on purpose: a static counter
    // would make every test see every other test's objects, and the suite would
    // start depending on the order ctest happens to run in.
    struct Ledger
    {
        int acquired {0};          // times an instance started owning
        int released {0};          // times an instance stopped owning, for any reason
        int copies {0};
        int moves {0};
        int destructor_runs {0};   // times ~Tracked ran on an owning instance

        // Anything other than zero at the end of a scope means a resource was
        // leaked; below zero means one was let go twice.
        int live() const { return acquired - released; }
    };

    // A value type whose lifetime is observable from outside.
    //
    // It models what Bytes does - a default instance owns nothing, a moved-from
    // instance owns nothing - so it can stand in wherever a test needs to see
    // whether a container really let go of what it held. RingBuffer putting a
    // popped slot back to T() is invisible with an int and obvious here.
    class Tracked
    {
    public:
        Tracked() :
            _ledger(nullptr),
            _id(0)
        {
        }

        Tracked(Ledger& ledger, int id) :
            _ledger(&ledger),
            _id(id)
        {
            ++_ledger->acquired;
        }

        Tracked(const Tracked& other) :
            _ledger(other._ledger),
            _id(other._id)
        {
            if (_ledger != nullptr)
            {
                ++_ledger->acquired;
                ++_ledger->copies;
            }
        }

        // noexcept because Holder refuses anything that is not.
        Tracked(Tracked&& other) noexcept :
            _ledger(other._ledger),
            _id(other._id)
        {
            if (_ledger != nullptr)
            {
                ++_ledger->acquired;
                ++_ledger->moves;
            }
            other.letGo();
        }

        Tracked& operator=(const Tracked& other)
        {
            if (this != &other)
            {
                letGo();

                _ledger = other._ledger;
                _id     = other._id;

                if (_ledger != nullptr)
                {
                    ++_ledger->acquired;
                    ++_ledger->copies;
                }
            }
            return *this;
        }

        Tracked& operator=(Tracked&& other) noexcept
        {
            if (this != &other)
            {
                letGo();

                _ledger = other._ledger;
                _id     = other._id;

                if (_ledger != nullptr)
                {
                    ++_ledger->acquired;
                    ++_ledger->moves;
                }
                other.letGo();
            }
            return *this;
        }

        ~Tracked()
        {
            if (_ledger != nullptr)
            {
                ++_ledger->destructor_runs;
            }
            letGo();
        }

        bool owns() const { return _ledger != nullptr; }
        int id() const { return _id; }

    private:
        void letGo()
        {
            if (_ledger != nullptr)
            {
                ++_ledger->released;
                _ledger = nullptr;
            }
        }

        Ledger* _ledger;
        int     _id;
    };
}

#endif // KIOTTY_TEST_TRACKED_H
