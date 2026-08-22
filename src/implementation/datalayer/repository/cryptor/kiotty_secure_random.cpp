#include "kiotty_secure_random.h"

#if defined(_WIN32)
#  if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <sys/random.h>
#  include <cerrno>
#endif

namespace kiotty
{
#if defined(_WIN32)
    bool SecureRandom::fill(ByteSpan out)
    {
        if (out.size() == 0)
        {
            return true;
        }

        const NTSTATUS status = BCryptGenRandom(nullptr, out.data(),
                                                static_cast<ULONG>(out.size()),
                                                BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return status >= 0;
    }
#else
    bool SecureRandom::fill(ByteSpan out)
    {
        size_t filled = 0;

        while (filled < out.size())
        {
            const ssize_t got = getrandom(out.data() + filled, out.size() - filled, 0);

            if (got < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return false;
            }
            filled += static_cast<size_t>(got);
        }
        return true;
    }
#endif
}
