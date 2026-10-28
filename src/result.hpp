#ifndef NYLON_RESULT_HPP
#define NYLON_RESULT_HPP

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace nylon
{

struct error_t
{
	std::string message;
	std::uint32_t code = 0;
};

class status_t
{
public:
	static status_t success() noexcept
	{
		return status_t();
	}

	static status_t failure(std::string message, std::uint32_t code = 0)
	{
		return status_t(error_t{ std::move(message), code });
	}

	[[nodiscard]] bool ok() const noexcept
	{
		return !error_.has_value();
	}

	[[nodiscard]] const error_t& error() const noexcept
	{
		assert(error_.has_value());
		return *error_;
	}

private:
	status_t() noexcept = default;
	explicit status_t(error_t error)
		: error_(std::move(error))
	{
	}

	std::optional<error_t> error_;
};

template <typename T>
class result_t
{
public:
	static result_t success(T value)
	{
		return result_t(std::move(value));
	}

	static result_t failure(std::string message, std::uint32_t code = 0)
	{
		return result_t(error_t{ std::move(message), code });
	}

	[[nodiscard]] bool ok() const noexcept
	{
		return std::holds_alternative<T>(storage_);
	}

	[[nodiscard]] T& value() & noexcept
	{
		assert(ok());
		return std::get<T>(storage_);
	}

	[[nodiscard]] const T& value() const & noexcept
	{
		assert(ok());
		return std::get<T>(storage_);
	}

	[[nodiscard]] T&& value() && noexcept
	{
		assert(ok());
		return std::move(std::get<T>(storage_));
	}

	[[nodiscard]] const error_t& error() const noexcept
	{
		assert(!ok());
		return std::get<error_t>(storage_);
	}

private:
	explicit result_t(T value)
		: storage_(std::move(value))
	{
	}

	explicit result_t(error_t error)
		: storage_(std::move(error))
	{
	}

	std::variant<T, error_t> storage_;
};

}

#endif
