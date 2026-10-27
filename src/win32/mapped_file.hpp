#ifndef NYLON_WIN32_MAPPED_FILE_HPP
#define NYLON_WIN32_MAPPED_FILE_HPP

#include "error.hpp"
#include "resource.hpp"

#include <Windows.h>

#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>

namespace nylon::win32
{
	class mapped_file_t final
	{
	public:
		explicit mapped_file_t( const std::filesystem::path& path )
		{
			file_.reset( ::CreateFileW(
				path.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
				nullptr ) );

			if ( !file_ )
			{
				throw error( "CreateFileW" );
			}

			LARGE_INTEGER size{};

			require(
				::GetFileSizeEx( file_.get(), &size ),
				"GetFileSizeEx" );

			if ( size.QuadPart <= 0 )
			{
				throw std::runtime_error( "cannot map an empty file" );
			}

			if ( static_cast<unsigned long long>( size.QuadPart ) >
				static_cast<unsigned long long>( std::numeric_limits<std::size_t>::max() ) )
			{
				throw std::overflow_error( "file is too large to map into this process" );
			}

			size_ = static_cast<std::size_t>( size.QuadPart );

			mapping_.reset( ::CreateFileMappingW(
				file_.get(),
				nullptr,
				PAGE_READONLY,
				0,
				0,
				nullptr ) );

			if ( !mapping_ )
			{
				throw error( "CreateFileMappingW" );
			}

			view_ = ::MapViewOfFile(
				mapping_.get(),
				FILE_MAP_READ,
				0,
				0,
				0 );

			if ( view_ == nullptr )
			{
				throw error( "MapViewOfFile" );
			}
		}

		mapped_file_t( const mapped_file_t& ) = delete;
		mapped_file_t& operator=( const mapped_file_t& ) = delete;
		mapped_file_t( mapped_file_t&& ) = delete;
		mapped_file_t& operator=( mapped_file_t&& ) = delete;

		~mapped_file_t()
		{
			if ( view_ != nullptr )
			{
				::UnmapViewOfFile( view_ );
			}
		}

		[[nodiscard]] std::span<const std::byte> bytes() const noexcept
		{
			return
			{
				static_cast<const std::byte*>( view_ ),
				size_
			};
		}

	private:
		handle_t file_;
		handle_t mapping_;
		void* view_ = nullptr;
		std::size_t size_ = 0;
	};
}

#endif
