#ifndef NYLON_WIN32_PRIVILEGE_HPP
#define NYLON_WIN32_PRIVILEGE_HPP

#include "error.hpp"
#include "resource.hpp"

#include <Windows.h>

#include <string>

namespace nylon::win32
{
	inline void enable_privilege( const wchar_t* name )
	{
		HANDLE raw_token = nullptr;

		require(
			::OpenProcessToken(
				::GetCurrentProcess(),
				TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
				&raw_token ),
			"OpenProcessToken" );

		handle_t token( raw_token );

		LUID luid{};

		require(
			::LookupPrivilegeValueW( nullptr, name, &luid ),
			"LookupPrivilegeValueW" );

		TOKEN_PRIVILEGES privileges{};
		privileges.PrivilegeCount = 1;
		privileges.Privileges[0].Luid = luid;
		privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

		::SetLastError( ERROR_SUCCESS );

		require(
			::AdjustTokenPrivileges(
				token.get(),
				FALSE,
				&privileges,
				sizeof( privileges ),
				nullptr,
				nullptr ),
			"AdjustTokenPrivileges" );

		if ( ::GetLastError() == ERROR_NOT_ALL_ASSIGNED )
		{
			throw error( "AdjustTokenPrivileges", ERROR_NOT_ALL_ASSIGNED );
		}
	}
}

#endif
