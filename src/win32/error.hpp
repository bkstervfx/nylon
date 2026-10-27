#ifndef NYLON_WIN32_ERROR_HPP
#define NYLON_WIN32_ERROR_HPP

#include <Windows.h>

#include <stdexcept>
#include <string>

namespace nylon::win32
{
	[[nodiscard]] inline std::runtime_error error( const std::string& operation, DWORD code = ::GetLastError() )
	{
		return std::runtime_error( operation + " failed with Win32 error " + std::to_string( code ) );
	}

	inline void require( BOOL result, const std::string& operation )
	{
		if ( result == FALSE )
		{
			throw error( operation );
		}
	}
}

#endif
