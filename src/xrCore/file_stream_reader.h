#ifndef FILE_STREAM_READER_H
#define FILE_STREAM_READER_H

#include "stream_reader.h"

class CFileStreamReader : public CStreamReader
{
private:
	typedef CStreamReader inherited;

#if defined(XR_PLATFORM_WINDOWS)
	HANDLE m_file_handle;
#elif defined(XR_PLATFORM_POSIX)
	int m_file_handle;
#endif

public:
	virtual void construct(LPCSTR file_name, const u32& window_size);
	virtual void destroy();
};

#endif // FILE_STREAM_READER_H
