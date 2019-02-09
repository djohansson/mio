#ifndef MIO_MMAP_STREAMBUF_HEADER
#define MIO_MMAP_STREAMBUF_HEADER

#include <algorithm>
#include <streambuf>

#include "page.hpp"
#include "shared_mmap.hpp"

namespace mio
{

template<access_mode AccessMode, typename ByteT>
class basic_mmap_streambuf : public std::basic_streambuf<ByteT>
{
public:
	using base_type = std::basic_streambuf<ByteT>;
    using base_type::eback;
    using base_type::gptr;
    using base_type::egptr;
    using base_type::pbase;
    using base_type::pptr;
    using base_type::epptr;
    using base_type::setg;
    using base_type::setp;
    using base_type::gbump;
    using base_type::pbump;

    using char_type = typename base_type::char_type;
	using traits_type = typename base_type::traits_type;
	using int_type = typename traits_type::int_type;
	using pos_type = typename traits_type::pos_type;
	using off_type = typename traits_type::off_type;

    basic_mmap_streambuf() = delete;
    basic_mmap_streambuf(const basic_mmap_streambuf&) = default;
    basic_mmap_streambuf(basic_mmap_streambuf&&) = default;
    basic_mmap_streambuf& operator=(const basic_mmap_streambuf&) = default;
    basic_mmap_streambuf& operator=(basic_mmap_streambuf&&) = default;

	basic_mmap_streambuf(basic_mmap<AccessMode, ByteT>&& m)
    : myMap(std::move(m))
    {
        reset();
    }

    basic_mmap_streambuf(const basic_shared_mmap<AccessMode, ByteT>& m)
    : myMap(m)
    {
        reset();
    }
    
#ifdef __cpp_exceptions
    ~basic_mmap_streambuf() noexcept(false)
#else
    ~basic_mmap_streambuf()
#endif
    {
        if constexpr (AccessMode == access_mode::write)
        {
            std::error_code error;
            myMap.truncate(pptr(), error);
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
                off += static_cast<off_type>(gptr() - myMap.data());
            else
                off += static_cast<off_type>(pptr() - myMap.data());
            break;

        case std::ios_base::end:
            off = myMap.size() - off;
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
        if (pos < 0 || !seekptr(const_cast<char_type*>(myMap.data()) + static_cast<ptrdiff_t>(pos), which))
		    return -1;

	    return pos;
    }

    int sync() override
    {
        if constexpr (AccessMode == access_mode::write)
        {
            std::error_code error;
            myMap.sync(error);
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
                myMap.remap(make_offset_page_aligned(myMap.size() + make_offset_page_aligned(n) + mio::page_size()), error);
                if (!error)
                {
                    std::ptrdiff_t offset = pptr() - pbase();
                    setp(myMap.data(), myMap.data() + myMap.size());
                    pbump(offset);
                }
                // else throw something
            }

            std::copy(s, s + n, pptr());
            pbump(n);

            return n;
        }

        return 0;
    }

	std::streamsize xsgetn(char_type* s, std::streamsize n) override
	{
        // todo: generate underflow
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
                myMap.remap(make_offset_page_aligned(myMap.size() + mio::page_size()), error);
                if (!error)
                {
                    std::ptrdiff_t offset = pptr() - pbase();
                    setp(myMap.data(), myMap.data() + myMap.size());
                    pbump(offset);
                    return *pptr() = ch;
                }
                // else throw something
            }
        }

        setp(nullptr, nullptr);
		    
        return traits_type::eof();
    }

    int_type underflow() override
    {
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
            setp(myMap.data(), myMap.data() + myMap.size());

        setg(const_cast<char_type*>(myMap.data()),
             const_cast<char_type*>(myMap.data()),
             const_cast<char_type*>(myMap.data()) + myMap.size());
    }

    void* seekptr(void* ptr_, std::ios_base::openmode which)
    {
        char* ptr = static_cast<char*>(ptr_);
        
        if constexpr (AccessMode == access_mode::write)
        {
            if ((which & std::ios_base::out) && ptr != pptr())
            {
                if (ptr >= myMap.data() && ptr < epptr())
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
            if (ptr >= myMap.data() && ptr < egptr())
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

    basic_shared_mmap<AccessMode, ByteT> myMap;
};

} // namespace mio

#endif // MIO_MMAP_STREAMBUF_HEADER