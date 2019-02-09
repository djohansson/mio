#ifndef MIO_MMAP_STREAMBUF_HEADER
#define MIO_MMAP_STREAMBUF_HEADER

#include <algorithm>
#include <streambuf>

#include "page.hpp"
#include "shared_mmap.hpp"

namespace mio
{

// todo: mapped subranges?

template<access_mode AccessMode, typename ByteT>
class mmap_streambuf : public std::basic_streambuf<ByteT>, public basic_shared_mmap<AccessMode, ByteT>
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
    using mmap_type = basic_shared_mmap<AccessMode, ByteT>;
    using mmap_type::data;
    using mmap_type::size;
    using mmap_type::sync;
    using mmap_type::remap;
    using mmap_type::truncate;

public:

    using char_type = typename streambuf_type::char_type;
	using traits_type = typename streambuf_type::traits_type;
	using int_type = typename traits_type::int_type;
	using pos_type = typename traits_type::pos_type;
	using off_type = typename traits_type::off_type;

    mmap_streambuf() = delete;
    mmap_streambuf(const mmap_streambuf&) = default;
    mmap_streambuf(mmap_streambuf&&) = default;
    mmap_streambuf& operator=(const mmap_streambuf&) = default;
    mmap_streambuf& operator=(mmap_streambuf&&) = default;

	mmap_streambuf(basic_mmap<AccessMode, ByteT>&& m)
    : mmap_type(std::move(m))
    {
        reset();
    }

    mmap_streambuf(const basic_shared_mmap<AccessMode, ByteT>& m)
    : mmap_type(m)
    {
        reset();
    }
    
#ifdef __cpp_exceptions
    ~mmap_streambuf() noexcept(false)
#else
    ~mmap_streambuf()
#endif
    {
        if constexpr (AccessMode == access_mode::write)
        {
            std::error_code error;
            truncate(pptr(), error);
            if (error)
            {
            #ifdef __cpp_exceptions
                throw std::system_error(error);
            #else
                perror("truncate");
            #endif
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
            if (which & std::ios_base::in)
                off += static_cast<off_type>(gptr() - data());
            else
                off += static_cast<off_type>(pptr() - data());
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
        {
            std::error_code error;
            sync(error);
            return error.value();
        }

        return 0;
    }

    std::streamsize xsputn(const char_type* s, std::streamsize n) override 
    {
        if constexpr (AccessMode == access_mode::write)
        {
            if (epptr() - pptr() < n)
            {
                std::error_code error;
                remap(make_offset_page_aligned(size() + make_offset_page_aligned(n) + mio::page_size()), error);
                if (!error)
                {
                    std::ptrdiff_t offset = pptr() - pbase();
                    setp(data(), data() + size());
                    pbump(offset);
                }
                // else throw something?
            }

            std::copy(s, s + n, pptr());
            pbump(n);

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
                std::error_code error;
                remap(make_offset_page_aligned(size() + mio::page_size()), error);
                if (!error)
                {
                    std::ptrdiff_t offset = pptr() - pbase();
                    setp(data(), data() + size());
                    pbump(offset);
                    return *pptr() = ch;
                }
                // else throw something?
            }
        }

        setp(nullptr, nullptr);
		    
        return traits_type::eof();
    }

    int_type underflow() override
    {
        // needs remap if subrange is mapped
        return (gptr() >= egptr() ? traits_type::eof() : traits_type::to_int_type(*gptr()));
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
    void reset()
    {
        if constexpr (AccessMode == access_mode::write)
            setp(data(), data() + size());

        setg(const_cast<char_type*>(data()),
             const_cast<char_type*>(data()),
             const_cast<char_type*>(data()) + size());
    }

    void* seekptr(void* ptr_, std::ios_base::openmode which)
    {
        char* ptr = static_cast<char*>(ptr_);
        
        if constexpr (AccessMode == access_mode::write)
        {
            if ((which & std::ios_base::out) && ptr != pptr())
            {
                if (ptr >= data() && ptr < epptr())
                {
                    setp(ptr, epptr());
                }
                else
                {
                    return nullptr;
                }
            }
        }

        if ((which & std::ios_base::in) && ptr != gptr())
        {
            if (ptr >= data() && ptr < egptr())
            {
                setg(eback(), ptr, egptr());
            }
            else
            {
                return nullptr;
            }
        }

        return ptr;
    }
};

} // namespace mio

#endif // MIO_MMAP_STREAMBUF_HEADER