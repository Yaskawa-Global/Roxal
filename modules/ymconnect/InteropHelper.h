
#pragma once
class InteropHelper
{
public:
	static ICHAR8* ConvertStringToChar8(std::string_view str);
	static std::string ConvertChar8ToString(ICHAR8* str);
	static ICHAR8* ConvertStringVectorToChar8(const std::vector<std::string>& str);
	static bool ToBool(INT32 value);
	static INT32 ToInt32(bool value);

};
