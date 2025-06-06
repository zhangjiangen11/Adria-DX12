#include "FileSink.h"
#include "Core/Paths.h"      

namespace adria
{

	FileSink::FileSink(Char const* log_file, LogLevel log_level, Bool append_mode)
		: log_level{ log_level }
	{
		std::string full_path = paths::LogDir + log_file;
		Char const* mode = append_mode ? "a" : "w";
		log_handle = fopen(full_path.c_str(), mode);
	}

	FileSink::~FileSink()
	{
		if (log_handle)
		{
			fflush(log_handle); 
			fclose(log_handle);
			log_handle = nullptr; 
		}
	}

	void FileSink::Log(LogLevel level, Char const* entry, Char const* file, Uint32 line)
	{
		if (level < log_level || !log_handle)
		{
			return;
		}

		std::string time_str = GetLogTime();
		std::string line_info_str = LineInfoToString(file, line);
		std::string level_str = LevelToString(level);
		fprintf(log_handle, "%s%s%s%s\n",
			time_str.c_str(),
			line_info_str.c_str(),
			level_str.c_str(),
			entry); 
	}

	void FileSink::Flush()
	{
		if (log_handle)
		{
			fflush(log_handle); 
		}
	}

} 