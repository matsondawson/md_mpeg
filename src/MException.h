#pragma once

#include <utility>
// Used by cthrow
#include <sstream>

#define streamHex(__w) "0x" << std::hex << std::setfill('0') << std::setw(__w)

#define cthrow(__type, __x) {std::ostringstream ex; ex << __x; throw __type(ex.str());}

#define customException(__type) \
	struct __type : public StringException { \
		__type(const std::string &ss) : StringException(ss) {}\
	};


struct StringException : public std::exception {
private:
	std::string s;

public:
	explicit
	StringException(const std::string &ss) : s(ss) {
	}

	~StringException() noexcept override = default;

	[[nodiscard]] const char *what() const noexcept override { return s.c_str(); }
};

