#pragma once

#include "domain/error.hpp"

#include <cstdlib>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace music_dl::domain {

namespace detail {
// Reading the wrong alternative of a Result is a programming error, not a runtime
// condition, so it terminates instead of throwing across module boundaries.
[[noreturn]] inline void bad_result_access() noexcept { std::abort(); }
} // namespace detail

// Minimal expected-like type for C++20 (std::expected is C++23).
template <class T> class [[nodiscard]] Result {
    static_assert(!std::is_same_v<std::remove_cvref_t<T>, Error>, "Result<Error> is ambiguous");

public:
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : data_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & {
        if (!has_value()) {
            detail::bad_result_access();
        }
        return std::get<0>(data_);
    }
    [[nodiscard]] const T& value() const& {
        if (!has_value()) {
            detail::bad_result_access();
        }
        return std::get<0>(data_);
    }
    [[nodiscard]] T&& value() && {
        if (!has_value()) {
            detail::bad_result_access();
        }
        return std::get<0>(std::move(data_));
    }

    [[nodiscard]] const Error& error() const& {
        if (has_value()) {
            detail::bad_result_access();
        }
        return std::get<1>(data_);
    }
    [[nodiscard]] Error&& error() && {
        if (has_value()) {
            detail::bad_result_access();
        }
        return std::get<1>(std::move(data_));
    }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

private:
    std::variant<T, Error> data_;
};

template <> class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const Error& error() const& {
        if (has_value()) {
            detail::bad_result_access();
        }
        return *error_;
    }

private:
    std::optional<Error> error_;
};

inline Result<void> ok() { return {}; }

} // namespace music_dl::domain
