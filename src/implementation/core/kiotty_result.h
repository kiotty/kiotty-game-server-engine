#if !defined(KIOTTY_RESULT_H)
#define KIOTTY_RESULT_H

#include <cassert>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace kiotty
{
    template<typename ErrorCode, typename Value>
    class Result
    {
        static_assert(
            std::is_enum<ErrorCode>::value || std::is_integral<ErrorCode>::value,
            "kiotty::Result: ErrorCode must be an enum or an integral type.");
        static_assert(
            !std::is_same<ErrorCode, bool>::value,
            "kiotty::Result: ErrorCode must not be bool.");

    public:
        struct Success {};
        struct Fail {};

        Result(const Success&, const Value& value) :
            _code(static_cast<ErrorCode>(0)),
            _value(value)
            {}

        Result(const Success&, Value&& value) :
            _code(static_cast<ErrorCode>(0)),
            _value(std::move(value))
            {}

        Result(const Fail&, ErrorCode error_code) :
            _code(error_code),
            _dummy()
            {}

        Result(const Result& other) :
            _code(other._code),
            _dummy()
        {
            if (other.isOk())
            {
                construct(other._value);
            }
        }

        Result(Result&& other) :
            _code(other._code),
            _dummy()
        {
            if (other.isOk())
            {
                construct(std::move(other._value));
            }
        }

        ~Result()
        {
            destroy();
        }

        Result& operator=(const Result& other)
        {
            if (this != &other)
            {
                assign(other);
            }
            return *this;
        }

        Result& operator=(Result&& other)
        {
            if (this != &other)
            {
                assign(std::move(other));
            }
            return *this;
        }

        explicit operator bool() const { return _code != static_cast<ErrorCode>(0); }

        bool isOk() const { return _code == static_cast<ErrorCode>(0); }

        ErrorCode code() const { return _code; }

        Value& value() { return _value; }
        const Value& value() const { return _value; }

    private:
        template<typename Source>
        void construct(Source&& source)
        {
            ::new (static_cast<void*>(std::addressof(_value)))
                Value(std::forward<Source>(source));
        }

        void destroy()
        {
            if (isOk())
            {
                _value.~Value();
            }
        }

        template<typename Other>
        void assign(Other&& other)
        {
            const bool other_is_ok = other.isOk();
            if (isOk() && other_is_ok)
            {
                _value = std::forward<Other>(other)._value;
            }
            else if (isOk())
            {
                _value.~Value();
            }
            else if (other_is_ok)
            {
                construct(std::forward<Other>(other)._value);
            }
            _code = other._code;
        }

        ErrorCode _code;

        union
        {
            char  _dummy;
            Value _value;
        };
    };

    template<typename ErrorCode, typename Value>
    class Result<ErrorCode, Value&>
    {
        static_assert(
            std::is_enum<ErrorCode>::value || std::is_integral<ErrorCode>::value,
            "kiotty::Result: ErrorCode must be an enum or an integral type.");
        static_assert(
            !std::is_same<ErrorCode, bool>::value,
            "kiotty::Result: ErrorCode must not be bool.");

    public:
        struct Success {};
        struct Fail {};

        Result(const Success&, Value& value) :
            _code(static_cast<ErrorCode>(0)),
            _value(std::addressof(value))
            {}

        Result(const Fail&, ErrorCode error_code) :
            _code(error_code),
            _value(nullptr)
            {}

        explicit operator bool() const { return _code != static_cast<ErrorCode>(0); }

        bool isOk() const { return _code == static_cast<ErrorCode>(0); }

        ErrorCode code() const { return _code; }

        Value& value() const
        {
            assert(_value != nullptr && "kiotty::Result<E, T&>::value() on a failed result");
            return *_value;
        }

    private:
        ErrorCode _code;
        Value*    _value;
    };

    namespace detail
    {
        template<typename Value>
        class OkHolder
        {
        public:
            explicit OkHolder(const Value& value) : _value(value) {}
            explicit OkHolder(Value&& value) : _value(std::move(value)) {}

            template<typename ErrorCode>
            operator Result<ErrorCode, Value>() const
            {
                typedef Result<ErrorCode, Value> ResultType;
                return ResultType(typename ResultType::Success(), _value);
            }

        private:
            Value _value;
        };

        template<typename Value>
        class OkRefHolder
        {
        public:
            explicit OkRefHolder(Value& value) : _value(std::addressof(value)) {}

            template<typename ErrorCode>
            operator Result<ErrorCode, Value&>() const
            {
                typedef Result<ErrorCode, Value&> ResultType;
                return ResultType(typename ResultType::Success(), *_value);
            }

        private:
            Value* _value;
        };

        template<typename ErrorCode>
        class FailHolder
        {
        public:
            explicit FailHolder(ErrorCode error_code) : _code(error_code) {}

            template<typename Value>
            operator Result<ErrorCode, Value>() const
            {
                typedef Result<ErrorCode, Value> ResultType;
                return ResultType(typename ResultType::Fail(), _code);
            }

        private:
            ErrorCode _code;
        };
    }

    template<typename Value>
    detail::OkHolder<typename std::decay<Value>::type> ok(Value&& value)
    {
        typedef typename std::decay<Value>::type ValueType;
        return detail::OkHolder<ValueType>(std::forward<Value>(value));
    }

    template<typename Value>
    detail::OkRefHolder<Value> okRef(Value& value)
    {
        return detail::OkRefHolder<Value>(value);
    }

    template<typename ErrorCode>
    detail::FailHolder<ErrorCode> error(ErrorCode error_code)
    {
        return detail::FailHolder<ErrorCode>(error_code);
    }
}

#endif
