#include "xml.hpp"

#include <Shlwapi.h>

#include <limits>
#include <stdexcept>
#include <string>

namespace
{
	void read_stream(
		IStream* stream,
		const nylon::xml::start_element_fn_t& on_start_element,
		const nylon::xml::text_fn_t& on_text,
		const nylon::xml::end_element_fn_t& on_end_element )
	{
		nylon::win32::com_ptr_t<IXmlReader> reader;
		const HRESULT reader_result = ::CreateXmlReader(
			__uuidof( IXmlReader ),
			reinterpret_cast<void**>( reader.put() ),
			nullptr );

		if ( FAILED( reader_result ) )
		{
			throw std::runtime_error( "CreateXmlReader failed" );
		}

		if ( FAILED( reader->SetProperty(
			XmlReaderProperty_DtdProcessing,
			DtdProcessing_Prohibit ) ) )
		{
			throw std::runtime_error( "IXmlReader::SetProperty failed" );
		}

		if ( FAILED( reader->SetInput( stream ) ) )
		{
			throw std::runtime_error( "IXmlReader::SetInput failed" );
		}

		XmlNodeType node_type = XmlNodeType_None;
		std::wstring current_element;

		for ( ;; )
		{
			const HRESULT read_result = reader->Read( &node_type );

			if ( read_result == S_FALSE )
			{
				break;
			}

			if ( FAILED( read_result ) )
			{
				throw std::runtime_error( "IXmlReader::Read failed" );
			}

			if ( node_type == XmlNodeType_Element )
			{
				const wchar_t* local_name = nullptr;
				UINT local_name_length = 0;

				if ( FAILED( reader->GetLocalName( &local_name, &local_name_length ) ) )
				{
					throw std::runtime_error( "IXmlReader::GetLocalName failed" );
				}

				current_element.assign( local_name, local_name_length );

				const auto attribute = [reader_ptr = reader.get()]( std::wstring_view wanted )
				{
					if ( reader_ptr->MoveToFirstAttribute() != S_OK )
					{
						return std::wstring{};
					}

					do
					{
						const wchar_t* name = nullptr;
						UINT name_length = 0;
						const wchar_t* value = nullptr;
						UINT value_length = 0;

						if ( FAILED( reader_ptr->GetLocalName( &name, &name_length ) ) ||
							FAILED( reader_ptr->GetValue( &value, &value_length ) ) )
						{
							throw std::runtime_error( "IXmlReader attribute read failed" );
						}

						if ( std::wstring_view( name, name_length ) == wanted )
						{
							const std::wstring result( value, value_length );
							reader_ptr->MoveToElement();
							return result;
						}
					}
					while ( reader_ptr->MoveToNextAttribute() == S_OK );

					reader_ptr->MoveToElement();
					return std::wstring{};
				};

				on_start_element( current_element, attribute );
				continue;
			}

			if ( on_text &&
				( node_type == XmlNodeType_Text ||
					node_type == XmlNodeType_CDATA ||
					node_type == XmlNodeType_Whitespace ||
					node_type == XmlNodeType_SignificantWhitespace ) )
			{
				const wchar_t* value = nullptr;
				UINT value_length = 0;

				if ( FAILED( reader->GetValue( &value, &value_length ) ) )
				{
					throw std::runtime_error( "IXmlReader::GetValue failed" );
				}

				if ( !current_element.empty() && value_length != 0 )
				{
					on_text(
						current_element,
						std::wstring_view( value, value_length ) );
				}

				continue;
			}

			if ( node_type == XmlNodeType_EndElement )
			{
				if ( on_end_element )
				{
					const wchar_t* local_name = nullptr;
					UINT local_name_length = 0;

					if ( FAILED( reader->GetLocalName( &local_name, &local_name_length ) ) )
					{
						throw std::runtime_error( "IXmlReader::GetLocalName failed for end element" );
					}

					on_end_element( std::wstring_view( local_name, local_name_length ) );
				}

				current_element.clear();
			}
		}
	}
}

namespace nylon::xml
{
	void read(
		const std::filesystem::path& path,
		const start_element_fn_t& on_start_element,
		const text_fn_t& on_text,
		const end_element_fn_t& on_end_element )
	{
		win32::com_ptr_t<IStream> stream;
		const HRESULT stream_result = ::SHCreateStreamOnFileEx(
			path.c_str(),
			STGM_READ | STGM_SHARE_DENY_NONE,
			FILE_ATTRIBUTE_NORMAL,
			FALSE,
			nullptr,
			stream.put() );

		if ( FAILED( stream_result ) )
		{
			throw std::runtime_error( "SHCreateStreamOnFileEx failed" );
		}

		read_stream( stream.get(), on_start_element, on_text, on_end_element );
	}

	void read(
		std::span<const std::byte> data,
		const start_element_fn_t& on_start_element,
		const text_fn_t& on_text,
		const end_element_fn_t& on_end_element )
	{
		if ( data.size_bytes() > static_cast<std::size_t>( std::numeric_limits<UINT>::max() ) )
		{
			throw std::overflow_error( "XML buffer is too large for SHCreateMemStream" );
		}

		IStream* raw_stream = ::SHCreateMemStream(
			reinterpret_cast<const BYTE*>( data.data() ),
			static_cast<UINT>( data.size_bytes() ) );

		if ( raw_stream == nullptr )
		{
			throw std::runtime_error( "SHCreateMemStream failed" );
		}

		win32::com_ptr_t<IStream> stream;
		stream.reset( raw_stream );
		read_stream( stream.get(), on_start_element, on_text, on_end_element );
	}
}
