#include "common.hpp"
//
//
#include "scan/scan.hpp"
//
#include <iostream>
#include <limits>
//
//
namespace nylon::internal
{
	//
	void print_error( const error_t& error )
	{
		std::cerr << "error: " << error.message;
		//
		if ( error.code != 0 )
		{
			std::cerr << " (" << error.code << ")";
		}
		//
		std::cerr << '\n';
	}
	//
	void print_usage()
	{
		std::cout
			<< "Nylon Windows image auditor\n\n"
			<< "usage:\n"
			<< "  nylon.exe scan <expanded-windows-image>\n"
			<< "  nylon.exe scan <windows.iso> [--index <number>]\n";
	}
	//
	result_t<uint32_t> parse_index( std::wstring_view value )
	{
		if ( value.empty() )
		{
			return result_t<uint32_t>::failure( "Image index cannot be empty" );
		}
		//
		uint64_t result = 0;
		//
		for ( const wchar_t character : value )
		{
			if ( character < L'0' || character > L'9' )
			{
				return result_t<uint32_t>::failure( "Image index must be a positive integer" );
			}
			//
			result = result * 10U + static_cast< uint64_t >( character - L'0' );
			//
			if ( result > std::numeric_limits<uint32_t>::max() )
			{
				return result_t<uint32_t>::failure( "Image index is too large" );
			}
		}
		//
		if ( result == 0 )
		{
			return result_t<uint32_t>::failure( "Image index must be greater than zero" );
		}
		//
		return result_t<uint32_t>::success( static_cast< uint32_t >( result ) );
	}
	//
	result_t<scan::options_t> parse_options( int count, wchar_t** arguments )
	{
		if ( count != 3 && count != 5 )
		{
			return result_t<scan::options_t>::failure( "Invalid command line" );
		}
		//
		if ( std::wstring_view( arguments[ 1 ] ) != L"scan" )
		{
			return result_t<scan::options_t>::failure( "Unknown command" );
		}
		//
		scan::options_t options;
		options.input = arguments[ 2 ];
		//
		if ( count == 5 )
		{
			if ( std::wstring_view( arguments[ 3 ] ) != L"--index" )
			{
				return result_t<scan::options_t>::failure( "Expected --index before image number" );
			}
			//
			auto index = parse_index( arguments[ 4 ] );
			//
			if ( !index.ok() )
			{
				return result_t<scan::options_t>::failure( index.error().message, index.error().code );
			}
			//
			options.image_index = index.value();
		}
		//
		return result_t<scan::options_t>::success( std::move( options ) );
	}
	//
}
//
//
int wmain( int count, wchar_t** arguments )
{
	auto options = nylon::internal::parse_options( count, arguments );
	//
	if ( !options.ok() )
	{
		nylon::internal::print_usage();
		nylon::internal::print_error( options.error() );
		//
		return 2;
	}
	//
	const nylon::status_t result = nylon::scan::run( options.value() );
	//
	if ( !result.ok() )
	{
		nylon::internal::print_error( result.error() );
		//
		return 1;
	}
	//
	return 0;
}
