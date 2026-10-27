#ifndef NYLON_WIN32_PROCESS_HPP
#define NYLON_WIN32_PROCESS_HPP

#include "error.hpp"

#include <Windows.h>

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

namespace nylon::win32
{
	[[nodiscard]] inline std::filesystem::path executable_path()
	{
		std::vector<wchar_t> buffer( 32768U );

		if ( buffer.size() > static_cast<std::size_t>( std::numeric_limits<DWORD>::max() ) )
		{
			throw std::overflow_error( "executable path buffer exceeds Win32 limit" );
		}

		const DWORD length = ::GetModuleFileNameW(
			nullptr,
			buffer.data(),
			static_cast<DWORD>( buffer.size() ) );

		if ( length == 0 )
		{
			throw error( "GetModuleFileNameW" );
		}

		if ( length >= buffer.size() )
		{
			throw std::runtime_error( "executable path exceeds Nylon's path buffer" );
		}

		return std::filesystem::path( std::wstring( buffer.data(), length ) );
	}
}

#endif
