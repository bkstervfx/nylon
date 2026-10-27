#ifndef NYLON_WIN32_RESOURCE_HPP
#define NYLON_WIN32_RESOURCE_HPP

#include <Windows.h>

#include <utility>

namespace nylon::win32
{
	class handle_t final
	{
	public:
		handle_t() noexcept = default;

		explicit handle_t( HANDLE value ) noexcept
			: value_( value )
		{
		}

		handle_t( const handle_t& ) = delete;
		handle_t& operator=( const handle_t& ) = delete;

		handle_t( handle_t&& other ) noexcept
			: value_( std::exchange( other.value_, nullptr ) )
		{
		}

		handle_t& operator=( handle_t&& other ) noexcept
		{
			if ( this != &other )
			{
				reset();
				value_ = std::exchange( other.value_, nullptr );
			}

			return *this;
		}

		~handle_t()
		{
			reset();
		}

		[[nodiscard]] HANDLE get() const noexcept
		{
			return value_;
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
		}

		void reset( HANDLE value = nullptr ) noexcept
		{
			if ( value_ != nullptr && value_ != INVALID_HANDLE_VALUE )
			{
				::CloseHandle( value_ );
			}

			value_ = value;
		}

	private:
		HANDLE value_ = nullptr;
	};

	class hkey_t final
	{
	public:
		hkey_t() noexcept = default;

		explicit hkey_t( HKEY value ) noexcept
			: value_( value )
		{
		}

		hkey_t( const hkey_t& ) = delete;
		hkey_t& operator=( const hkey_t& ) = delete;

		hkey_t( hkey_t&& other ) noexcept
			: value_( std::exchange( other.value_, nullptr ) )
		{
		}

		hkey_t& operator=( hkey_t&& other ) noexcept
		{
			if ( this != &other )
			{
				reset();
				value_ = std::exchange( other.value_, nullptr );
			}

			return *this;
		}

		~hkey_t()
		{
			reset();
		}

		[[nodiscard]] HKEY get() const noexcept
		{
			return value_;
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return value_ != nullptr;
		}

		void reset( HKEY value = nullptr ) noexcept
		{
			if ( value_ != nullptr )
			{
				::RegCloseKey( value_ );
			}

			value_ = value;
		}

	private:
		HKEY value_ = nullptr;
	};

	class module_t final
	{
	public:
		module_t() noexcept = default;

		explicit module_t( HMODULE value ) noexcept
			: value_( value )
		{
		}

		module_t( const module_t& ) = delete;
		module_t& operator=( const module_t& ) = delete;

		module_t( module_t&& other ) noexcept
			: value_( std::exchange( other.value_, nullptr ) )
		{
		}

		module_t& operator=( module_t&& other ) noexcept
		{
			if ( this != &other )
			{
				reset();
				value_ = std::exchange( other.value_, nullptr );
			}

			return *this;
		}

		~module_t()
		{
			reset();
		}

		[[nodiscard]] HMODULE get() const noexcept
		{
			return value_;
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return value_ != nullptr;
		}

		void reset( HMODULE value = nullptr ) noexcept
		{
			if ( value_ != nullptr )
			{
				::FreeLibrary( value_ );
			}

			value_ = value;
		}

	private:
		HMODULE value_ = nullptr;
	};

	template <typename type_t>
	class com_ptr_t final
	{
	public:
		com_ptr_t() noexcept = default;

		com_ptr_t( const com_ptr_t& ) = delete;
		com_ptr_t& operator=( const com_ptr_t& ) = delete;

		com_ptr_t( com_ptr_t&& other ) noexcept
			: value_( std::exchange( other.value_, nullptr ) )
		{
		}

		com_ptr_t& operator=( com_ptr_t&& other ) noexcept
		{
			if ( this != &other )
			{
				reset();
				value_ = std::exchange( other.value_, nullptr );
			}

			return *this;
		}

		~com_ptr_t()
		{
			reset();
		}

		[[nodiscard]] type_t* get() const noexcept
		{
			return value_;
		}

		[[nodiscard]] type_t** put() noexcept
		{
			reset();
			return &value_;
		}

		[[nodiscard]] type_t* operator->() const noexcept
		{
			return value_;
		}

		[[nodiscard]] explicit operator bool() const noexcept
		{
			return value_ != nullptr;
		}

		void reset( type_t* value = nullptr ) noexcept
		{
			if ( value_ != nullptr )
			{
				value_->Release();
			}

			value_ = value;
		}

	private:
		type_t* value_ = nullptr;
	};
}

#endif
