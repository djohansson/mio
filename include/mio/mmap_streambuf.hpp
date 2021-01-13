#ifndef MIO_MMAP_STREAMBUF_HEADER
#define MIO_MMAP_STREAMBUF_HEADER

#include <cassert>
#include <algorithm>
#include <streambuf>

#include "page.hpp"
#include "mmap.hpp"

namespace mio
{

// todo: support all std::ios_base::openmode flags. right now it truncates.
// todo: mapped subranges?

template<access_mode AccessMode, typename ByteT = char>
class mmap_streambuf : public std::basic_streambuf<ByteT>, public basic_mmap<AccessMode, ByteT>
{
    using streambuf_type = std::basic_streambuf<ByteT>;
    using streambuf_type::eback;
    using streambuf_type::gptr;
    using streambuf_type::egptr;
    using streambuf_type::pbase;
    using streambuf_type::pptr;
    using streambuf_type::epptr;
    using streambuf_type::setg;
    using streambuf_type::setp;
    using streambuf_type::gbump;
    using streambuf_type::pbump;
    using mmap_type = basic_mmap<AccessMode, ByteT>;
    using mmap_type::data;
    using mmap_type::size;
    using mmap_type::remap;
    using mmap_type::sync;
    using mmap_type::truncate;

public:

    using char_type = typename streambuf_type::char_type;
	using traits_type = typename streambuf_type::traits_type;
	using int_type = typename traits_type::int_type;
	using pos_type = typename traits_type::pos_type;
	using off_type = typename traits_type::off_type;
    using size_type = typename mmap_type::size_type;

    mmap_streambuf() = delete;
    mmap_streambuf(const mmap_streambuf&) = default;
    mmap_streambuf(mmap_streambuf&&) = default;
    mmap_streambuf& operator=(const mmap_streambuf&) = default;
    mmap_streambuf& operator=(mmap_streambuf&&) = default;

	mmap_streambuf(basic_mmap<AccessMode, ByteT>&& m)
    : mmap_type(std::move(m))
    {
        resetptrs();
    }

    template<typename String>
    mmap_streambuf(const String& path, const size_type offset = 0, const size_type length = map_entire_file)
    : mmap_type(path, offset, length)
    {
        resetptrs();
    }

    virtual ~mmap_streambuf()
    {
        if constexpr (AccessMode == access_mode::write)
        {
            if (state.high_water > 0)
            {
                std::error_code error;
                truncate(state.high_water, error);
                assert(!error);
            }
        }
    }

protected:
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
        std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override
    {
        switch(dir)
        {
        case std::ios_base::beg:
            break;

        case std::ios_base::cur:
            if (which & std::ios_base::out)
                off += static_cast<off_type>(pptr() - pbase());
            else
                off += static_cast<off_type>(gptr() - eback());
            break;

        case std::ios_base::end:
            off = size() - off;
            break;

        default:
            return -1;
        }

        if (off < 0)
            return -1;

        return seekpos(off, which);
    }

    pos_type seekpos(pos_type pos,
        std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override
    {
        if (pos < 0 || !seekptr(const_cast<char_type*>(data()) + static_cast<ptrdiff_t>(pos), which))
		    return -1;

	    return pos;
    }

    int sync() override
    {
        if constexpr (AccessMode == access_mode::write)
            resetptrs();

        return 0;
    }

    std::streamsize xsputn(const char_type* s, std::streamsize n) override 
    {
        if constexpr (AccessMode == access_mode::write)
        {
            if (epptr() - pptr() < n)
            {
                std::error_code error;
                off_type poffset = pptr() - pbase();
                remap(std::max(2 * size(), page_size() + make_offset_page_aligned(static_cast<size_type>(poffset + n))), error);
                if (error)
                    throw std::system_error(std::move(error));

                resetptrs();
            }

            std::copy(s, s + n, pptr());
            pbump(n);
            phwset(pptr() - pbase());

            return n;
        }

        return 0;
    }

	std::streamsize xsgetn(char_type* s, std::streamsize n) override
	{
        // todo: generate underflow if subrange is mapped
		std::ptrdiff_t count = std::min(egptr() - gptr(), n);

		std::copy(gptr(), gptr() + count, s);
        gbump(count);

		return count;
	}

	int_type overflow(int_type ch = traits_type::eof()) override 
    {
        if (!traits_type::eq_int_type(ch, traits_type::eof()))
        {
            if constexpr (AccessMode == access_mode::write)
            {
                if (epptr() - pptr() < 1)
                {
                    std::error_code error;
                    off_type poffset = pptr() - pbase();
                    remap(std::max(2 * size(), page_size() + make_offset_page_aligned(static_cast<size_type>(poffset))), error);
                    if (error)
                        throw std::system_error(std::move(error));

                    resetptrs();
                }

                *pptr() = ch;
                pbump(1);
                phwset(pptr() - pbase());

                return ch;
            }
        }

        setp(nullptr, nullptr);
		    
        return traits_type::eof();
    }

    int_type underflow() override
    {
        // needs remap if subrange is mapped
        return (gptr() >= egptr() || gptr() < eback() ? traits_type::eof() : traits_type::to_int_type(*gptr()));
    }

    int_type pbackfail(int_type ch) override
    {
        if (gptr() == eback())
            return traits_type::eof();

        if (ch != traits_type::eof() && ch != gptr()[-1])
        {
            auto retval = gptr()[-1] = ch;
            setg(eback(), gptr() - 1, egptr());
            return traits_type::to_int_type(retval);
        }

        return traits_type::eof();
    }

    std::streamsize showmanyc() override
    {
        return egptr() - gptr();
    }

private:
    void resetptrs()
    {
        if constexpr (AccessMode == access_mode::write)
        {
            off_type poffset = pptr() - pbase();
            setp(data(), data() + size());
            pbump(poffset);
            phwset(poffset);

            off_type goffset = gptr() - eback();
            setg(const_cast<char_type*>(data()),
                const_cast<char_type*>(data()),
                const_cast<char_type*>(data()) + state.high_water);
            gbump(goffset);
        }
        else
        {
            off_type goffset = gptr() - eback();
            setg(const_cast<char_type*>(data()),
                const_cast<char_type*>(data()),
                const_cast<char_type*>(data()) + size());
            gbump(goffset);
        }
    }

    bool seekptr(void* ptr_, std::ios_base::openmode which)
    {
        char* ptr = static_cast<char*>(ptr_);
        
        if constexpr (AccessMode == access_mode::write)
        {
            if ((which & std::ios_base::out) && ptr != pptr())
            {
                if (ptr >= pbase() && ptr < epptr())
                {
                    off_type poffset = ptr - pbase();
                    setp(pbase(), epptr());
                    pbump(poffset);
                    phwset(poffset);
                }
                else
                {
                    return false;
                }
            }
        }

        if ((which & std::ios_base::in) && ptr != gptr())
        {
            if (ptr >= data() && ptr <= egptr())
                setg(eback(), ptr, egptr());
            else
                return false;
        }

        return true;
    }

    template<access_mode A = AccessMode>
    typename std::enable_if<A == access_mode::write, void>::type
    phwset(off_type poffset)
    {
        if (state.high_water < poffset)
            state.high_water = poffset;
    }

    struct ReadAccessState {};
    struct WriteAccessState { off_type high_water = 0; };
    std::conditional_t<AccessMode == access_mode::write, WriteAccessState, ReadAccessState> state;
};

using mmap_istreambuf = mmap_streambuf<access_mode::read>;
using mmap_iostreambuf = mmap_streambuf<access_mode::write>;
using mmap_ostreambuf = mmap_iostreambuf;

} // namespace mio

#endif // MIO_MMAP_STREAMBUF_HEADER
