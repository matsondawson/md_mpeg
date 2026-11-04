#pragma once

#include <cstdint>
#include <cstring>

#define FILENAME (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)
#define FILENAME_NOEXT (strchr(FILENAME, '.') ? strndup(FILENAME, strchr(FILENAME, '.') - FILENAME) : FILENAME)

#define debug_out if (false) std::cout << FILENAME_NOEXT << " - "
#define info_out std::cout << FILENAME_NOEXT << " - "

#define SHL(_value, _bits) ((_bits) == 32 ? 0 : ((_value) << (_bits)))
#define SHR(_value, _bits) ((_bits) == 32 ? 0 : ((_value) >> (_bits)))
