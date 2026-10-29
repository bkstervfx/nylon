#include "hash.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "win32.hpp"

namespace nylon::platform
{

namespace
{

class algorithm_handle_t
{
public:
	algorithm_handle_t() noexcept = default;
	explicit algorithm_handle_t(BCRYPT_ALG_HANDLE handle) noexcept
		: handle_(handle)
	{
	}

	~algorithm_handle_t()
	{
		reset();
	}

	algorithm_handle_t(const algorithm_handle_t&) = delete;
	algorithm_handle_t& operator=(const algorithm_handle_t&) = delete;

	algorithm_handle_t(algorithm_handle_t&& other) noexcept
		: handle_(std::exchange(other.handle_, nullptr))
	{
	}

	algorithm_handle_t& operator=(algorithm_handle_t&& other) noexcept
	{
		if (this != &other)
		{
			reset();
			handle_ = std::exchange(other.handle_, nullptr);
		}
		return *this;
	}

	[[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept
	{
		return handle_;
	}

	void reset() noexcept
	{
		if (handle_ != nullptr)
		{
			BCryptCloseAlgorithmProvider(handle_, 0);
			handle_ = nullptr;
		}
	}

private:
	BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class hash_handle_t
{
public:
	hash_handle_t() noexcept = default;
	explicit hash_handle_t(BCRYPT_HASH_HANDLE handle) noexcept
		: handle_(handle)
	{
	}

	~hash_handle_t()
	{
		reset();
	}

	hash_handle_t(const hash_handle_t&) = delete;
	hash_handle_t& operator=(const hash_handle_t&) = delete;

	hash_handle_t(hash_handle_t&& other) noexcept
		: handle_(std::exchange(other.handle_, nullptr))
	{
	}

	hash_handle_t& operator=(hash_handle_t&& other) noexcept
	{
		if (this != &other)
		{
			reset();
			handle_ = std::exchange(other.handle_, nullptr);
		}
		return *this;
	}

	[[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept
	{
		return handle_;
	}

	void reset() noexcept
	{
		if (handle_ != nullptr)
		{
			BCryptDestroyHash(handle_);
			handle_ = nullptr;
		}
	}

private:
	BCRYPT_HASH_HANDLE handle_ = nullptr;
};

result_t<ULONG> bcrypt_ulong_property(
	BCRYPT_HANDLE handle,
	LPCWSTR property,
	std::string context)
{
	ULONG value = 0;
	ULONG written = 0;
	const NTSTATUS status = BCryptGetProperty(
		handle,
		property,
		reinterpret_cast<PUCHAR>(&value),
		static_cast<ULONG>(sizeof(value)),
		&written,
		0);

	if (!BCRYPT_SUCCESS(status) || written != sizeof(value))
	{
		return result_t<ULONG>::failure(
			std::move(context),
			static_cast<std::uint32_t>(status));
	}

	return result_t<ULONG>::success(value);
}

}

result_t<std::string> sha256_file(const std::filesystem::path& path)
{
	auto native = native_path(path);
	if (!native.ok())
	{
		return result_t<std::string>::failure(native.error().message, native.error().code);
	}

	unique_handle_t file(CreateFileW(
		native.value().c_str(),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
		nullptr));

	if (!file)
	{
		const auto error = win32_error("CreateFileW failed while hashing input");
		return result_t<std::string>::failure(error.message, error.code);
	}

	BCRYPT_ALG_HANDLE raw_algorithm = nullptr;
	NTSTATUS status = BCryptOpenAlgorithmProvider(
		&raw_algorithm,
		BCRYPT_SHA256_ALGORITHM,
		nullptr,
		0);

	if (!BCRYPT_SUCCESS(status))
	{
		return result_t<std::string>::failure(
			"BCryptOpenAlgorithmProvider failed for SHA-256",
			static_cast<std::uint32_t>(status));
	}

	algorithm_handle_t algorithm(raw_algorithm);

	auto object_length = bcrypt_ulong_property(
		algorithm.get(),
		BCRYPT_OBJECT_LENGTH,
		"BCryptGetProperty failed for SHA-256 object length");
	if (!object_length.ok())
	{
		return result_t<std::string>::failure(
			object_length.error().message,
			object_length.error().code);
	}

	auto hash_length = bcrypt_ulong_property(
		algorithm.get(),
		BCRYPT_HASH_LENGTH,
		"BCryptGetProperty failed for SHA-256 hash length");
	if (!hash_length.ok())
	{
		return result_t<std::string>::failure(
			hash_length.error().message,
			hash_length.error().code);
	}

	if (hash_length.value() != 32U)
	{
		return result_t<std::string>::failure("SHA-256 provider returned an unexpected digest length");
	}

	std::vector<UCHAR> hash_object(object_length.value());
	BCRYPT_HASH_HANDLE raw_hash = nullptr;
	status = BCryptCreateHash(
		algorithm.get(),
		&raw_hash,
		hash_object.empty() ? nullptr : hash_object.data(),
		static_cast<ULONG>(hash_object.size()),
		nullptr,
		0,
		0);

	if (!BCRYPT_SUCCESS(status))
	{
		return result_t<std::string>::failure(
			"BCryptCreateHash failed for SHA-256",
			static_cast<std::uint32_t>(status));
	}

	hash_handle_t hash(raw_hash);
	std::vector<UCHAR> buffer(1024U * 1024U);

	for (;;)
	{
		DWORD bytes_read = 0;
		if (!ReadFile(
			file.get(),
			buffer.data(),
			static_cast<DWORD>(buffer.size()),
			&bytes_read,
			nullptr))
		{
			const auto error = win32_error("ReadFile failed while hashing input");
			return result_t<std::string>::failure(error.message, error.code);
		}

		if (bytes_read == 0)
		{
			break;
		}

		status = BCryptHashData(hash.get(), buffer.data(), bytes_read, 0);
		if (!BCRYPT_SUCCESS(status))
		{
			return result_t<std::string>::failure(
				"BCryptHashData failed while hashing input",
				static_cast<std::uint32_t>(status));
		}
	}

	std::array<UCHAR, 32> digest{};
	status = BCryptFinishHash(
		hash.get(),
		digest.data(),
		static_cast<ULONG>(digest.size()),
		0);

	if (!BCRYPT_SUCCESS(status))
	{
		return result_t<std::string>::failure(
			"BCryptFinishHash failed for SHA-256",
			static_cast<std::uint32_t>(status));
	}

	static constexpr char hex[] = "0123456789abcdef";
	std::string encoded;
	encoded.resize(digest.size() * 2U);

	for (std::size_t index = 0; index < digest.size(); ++index)
	{
		const unsigned value = digest[index];
		encoded[index * 2U] = hex[(value >> 4U) & 0xFU];
		encoded[index * 2U + 1U] = hex[value & 0xFU];
	}

	return result_t<std::string>::success(std::move(encoded));
}

}
