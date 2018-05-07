#ifndef UTIL__STRING_HH
#define UTIL__STRING_HH
#pragma once

#include <string>

namespace MATHUSLA {

namespace util { ///////////////////////////////////////////////////////////////////////////////

namespace string { /////////////////////////////////////////////////////////////////////////////

//__Split on Delimeters_________________________________________________________________________
template <class Range>
void split(const std::string& string,
           Range& tokens,
           const std::string& delimiters=" ") {
  const auto&& size = string.size();
  std::string::size_type position, previous = 0;
  while (previous <= size) {
    position = string.find_first_of(delimiters, previous);
    if (position == std::string::npos) position = size;
    tokens.emplace_back(string.data() + previous, position - previous);
    previous = position + 1;
  }
}
//----------------------------------------------------------------------------------------------

//__Split on New Lines__________________________________________________________________________
template <class Range>
void splitlines(const std::string& string,
                Range& tokens) {
  split(string, tokens, "\n\r");
}
//----------------------------------------------------------------------------------------------

//__Remove Leading and Trailing Spaces__________________________________________________________
inline void strip(std::string& string) {
  // TODO: implement!
}
//----------------------------------------------------------------------------------------------

} /* namespace string */ ///////////////////////////////////////////////////////////////////////

} /* namespace util */ /////////////////////////////////////////////////////////////////////////

} /* namespace MATHUSLA */

#endif /* UTIL__STRING_HH */