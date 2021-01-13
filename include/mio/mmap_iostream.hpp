#ifndef MIO_MMAP_IOSTREAM_HEADER
#define MIO_MMAP_IOSTREAM_HEADER

#include <iostream>

#include "mmap_streambuf.hpp"

namespace mio
{

class mmap_iostream : public std::iostream, public mmap_iostreambuf
{
public:

    template<typename String>
    mmap_iostream(const String& path, const size_type offset = 0, const size_type length = map_entire_file)
     : std::iostream(this)
     , mmap_iostreambuf(path, offset, length)
    {}
};

class mmap_istream : public std::istream, public mmap_istreambuf
{
public:

    template<typename String>
    mmap_istream(const String& path, const size_type offset = 0, const size_type length = map_entire_file)
     : std::istream(this)
     , mmap_istreambuf(path, offset, length)
    {}
};

class mmap_ostream : public std::ostream, public mmap_ostreambuf
{
public:

    template<typename String>
    mmap_ostream(const String& path, const size_type offset = 0, const size_type length = map_entire_file)
     : std::ostream(this)
     , mmap_ostreambuf(path, offset, length)
    {}
};

} // namespace mio

#endif // MIO_MMAP_IOSTREAM_HEADER
