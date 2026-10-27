#include "database/database.hpp"
#include "image/image.hpp"
#include "win32/process.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int wmain( int argc, wchar_t** argv )
{
	try
	{
		if ( argc > 2 )
		{
			std::wcerr << L"usage: nylon.exe [image-root]\n";
			return 2;
		}

		const std::filesystem::path requested_image =
			argc == 2 ? std::filesystem::path( argv[1] ) : std::filesystem::path{};

		const auto paths = nylon::image::resolve_paths(
			nylon::win32::executable_path(),
			requested_image );

		const auto database_path = paths.output_root / L"image.db";

		std::error_code remove_error;
		const bool removed = std::filesystem::remove( database_path, remove_error );

		if ( remove_error )
		{
			throw std::runtime_error( "could not replace the existing output database" );
		}

		(void)removed;

		std::wcout << L"Nylon Windows image auditor\n";
		std::wcout << L"image:  " << paths.image_root.native() << L"\n";
		std::wcout << L"output: " << database_path.native() << L"\n\n";

		nylon::database::database_t database( database_path );
		nylon::image::audit( paths, database );

		std::wcout << L"\nAudit complete.\n";
		return 0;
	}
	catch ( const std::exception& error )
	{
		std::cerr << "fatal: " << error.what() << '\n';
		return 1;
	}
}
