
#ifndef __UCOMM_BASE_DEFINES_H_____
#define __UCOMM_BASE_DEFINES_H_____

#include <stdio.h>
#include <assert.h>
#include <vector>
#include <string>
#include <ctime>
#if defined(WINCE) || defined(UNDER_CE) || defined(_WIN32_WCE)
#include <time.h>
#if (_MSC_VER < 1600)
#define nullptr NULL
#endif
#else
#include <sys/timeb.h>
#include <stdint.h>
#include <typeinfo>
#endif
#include <string.h>

#define MB_ERROR_UNKNOWN				(-1)

#define MB_BAD_DESERIALIZATION			0xAAAA0000
#define MB_SERIALIZATION_NOT_SUPPORTED	0xAAAA0001
#define	MB_BAD_OPERATION				0xAAAA0002
#define MB_BAD_INPUT					0xAAAA0003
#define MB_NOT_SUPPORTED				0xAAAA0004
#define MB_STL_EXCEPTION				0xAAAA0005
#define MB_UNKNOWN_EXCEPTION			0xAAAA0006
#define MB_QUEUE_FILE_NOT_AVAILABLE		0xAAAA0007
#define MB_ALREADY_DEQUEUED				0xAAAA0008
#define MB_ROUTEE_DISCONNECTED			0xAAAA0009
#define MB_REQUEST_ABORTED				0xAAAA000A

#define MAX_USERID_CHARS		255
#define	MAX_PASSWORD_CHARS		255

#define MILLISECONDS_PER_DAY         86400000

// 25569 == difference between epoch (1970/01/01:00/00/00) and variant date (1899/12/30:00/00/00)
#define DAYS_DIFF_EPOCH_VDATE   25569

namespace SPA {

    /**
     * Defines for supported compression options
     */
    typedef enum tagZipLevel {
        zlDefault = 0,
        zlBestSpeed = 1,
        zlBestCompression = 2
    } ZipLevel;

    /**
     * 
     */
    enum tagSocketOption {
        soTcpNoDelay = 1,
        soReuseAddr = 4,
        soKeepAlive = 8,
        soSndBuf = 0x1001, /* send buffer size */
        soRcvBuf = 0x1002, /* receive buffer size */
    };

    /**
     * 
     */
    enum tagSocketLevel {
        slTcp = 6,
        slSocket = 0xFFFF,
    };

    /**
     * Defines for supported operation systems
     */
    enum tagOperationSystem {
        osWin = 0,
        osApple = 1,
        osMac = osApple,
        osUnix = 2,
        osLinux = osUnix,
        osBSD = osUnix,
        osAndroid = 3,
        osWinCE = 4, /**< Old window pocket pc, ce or smart phone devices*/
        osWinPhone = osWinCE,
    };

    /**
     * Defines for MS COM object thread apartments for window platforms only
     */
    enum tagThreadApartment {
        /// no COM apartment involved
        taNone = 0,

        /// STA apartment
        taApartment = 1,

        /// MTA (free) or neutral apartments
        taFree = 2,
    };
};


#if defined(WIN32) || defined(_WIN64) || defined(WINDOWS) || defined(_WIN32_WCE) || defined(WIN32_WCE) || defined(WIN64)
#ifndef WIN32_64
#define WIN32_64
#endif
#define ftime   _ftime    

#include <atlbase.h>
#include <atlstr.h>

namespace SPA {
    typedef __int64 INT64;
    typedef unsigned __int64 UINT64;

    static double ToVariantDate(INT64 ms) {
        double dt(DAYS_DIFF_EPOCH_VDATE);
        INT64 days = ms / MILLISECONDS_PER_DAY;
        dt += days;
        double millseconds = (double) (ms % MILLISECONDS_PER_DAY);
        dt += (millseconds / MILLISECONDS_PER_DAY);
        return dt;
    }

    static INT64 ToEpoch(double vtDate) {
        vtDate += 0.5 / MILLISECONDS_PER_DAY; //rounded to ms
        vtDate -= DAYS_DIFF_EPOCH_VDATE;
        vtDate *= MILLISECONDS_PER_DAY;
        return (INT64) vtDate;
    }
};

#define U_MODULE_HIDDEN

#else
#include <stdlib.h>
#include <dlfcn.h>
#include <wchar.h>
#include <sys/time.h>

#define GetProcAddress dlsym
#define FreeLibrary dlclose

#define WINAPI
#define CALLBACK

typedef void* HINSTANCE;

#if defined(__ANDROID__) || defined(ANDROID)
namespace std {
    typedef decltype(nullptr) nullptr_t;
}
#endif

#ifndef BOOST_SYSTEM_NO_DEPRECATED
#define BOOST_SYSTEM_NO_DEPRECATED 1
#endif

namespace SPA {
    typedef int64_t INT64;
    typedef uint64_t UINT64;
};

#pragma pack(push,1)

typedef struct tagCY {
    int64_t int64;

    inline bool operator==(const tagCY & cy) const {
        return (::memcmp(this, &cy, sizeof (cy)) == 0);
    }
} CY;

#pragma pack(pop)

#pragma pack(push,1)

typedef struct tagDEC {
    unsigned short wReserved;

    union {

        struct {
            unsigned char scale;
            unsigned char sign;
        };
        unsigned short signscale;
    };
    unsigned int Hi32;

    union {

        struct {
            unsigned int Lo32;
            unsigned int Mid32;
        };
        uint64_t Lo64;
    };

    inline bool operator==(const tagDEC & dec) const {
        return (::memcmp(this, &dec, sizeof (dec)) == 0);
    }
} DECIMAL;
#pragma pack(pop)

static_assert(sizeof (tagDEC) == 16, "Bad tagDEC structure size!");

#if __GNUC__ >= 4
#define U_MODULE_HIDDEN __attribute__ ((visibility ("hidden")))
#else
#define U_MODULE_HIDDEN
#endif

#endif

#define INVALID_NUMBER		((SPA::UINT64)(~0))

#ifdef __cplusplus
extern "C" {
#endif

    void WINAPI SetLastCallInfo(const char *str);
    typedef void (WINAPI *PSetLastCallInfo)(const char *str);

#ifdef __cplusplus
}
#endif

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)

#define WCHAR32
typedef timeval SYSTEMTIME;
#else
#define WCHAR16
#endif

namespace SPA {

    /** 
     * Query operation system
     * @return The operation system
     */
    static inline tagOperationSystem GetOS() {
#if defined(__ANDROID__) || defined(ANDROID)
        return osAndroid;
#elif defined(linux) || defined(__linux) || defined(__linux__) || defined(__GNU__) || defined(__GLIBC__)
        return osLinux;
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
        return osBSD;
#elif defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
        return osWin;
#elif defined(UNDER_CE) || defined(_WIN32_WCE) || defined(WIN32_WCE) || defined(WINCE)
        return osWinCE;
#elif defined(_WIN64) || defined(WIN64)
        return osWin;
#elif defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)
        return osApple;
#else
        return osUnknown;
#endif
    }

    /** 
     * Check if current operation system is big endian
     * @return True for big endian and false for little endian
     */
    static bool IsBigEndian() {
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
        return false;
#elif defined(UNDER_CE) || defined(_WIN32_WCE) || defined(WIN32_WCE)
        return false;
#elif defined(_WIN64) || defined(WIN64)
        return false;
#elif defined(__ANDROID__) || defined(ANDROID)
        return false;
#elif defined(__sparc) || defined(__sparc__) \
	|| defined(_POWER) || defined(__powerpc__) \
	|| defined(__ppc__) || defined(__hpux) || defined(__hppa) \
	|| defined(_MIPSEB) \
	|| defined(__s390__)
        return true;
#else
        return false;
#endif
    }

    /** 
     * Query the size of wide char in byte from a given operation system
     * @param os Operation system
     * @return The size of wide char in byte
     */
    static inline unsigned char GetWCharSize(tagOperationSystem os) {
        switch (os) {
            case osWin:
            case osWinCE:
                return 2;
                break;
            default:
                break;
        }
        return 4;
    }

    /** 
     * Query if the current system is one of MS window platforms
     * @param os Operation system
     * @return True or false
     */
    static inline bool IsWinOs(tagOperationSystem os) {
        switch (os) {
            case osWin:
            case osWinCE:
                return true;
                break;
            default:
                break;
        }
        return false;
    }
};

#define MY_OPERATION_SYSTEM  (SPA::GetOS()) 

namespace SPA {

#ifdef WCHAR16
    typedef wchar_t UTF16;
#else
    typedef unsigned short UTF16;
#endif

    //The following functions atoxxx work correctly for standard and normal number strings without any preventions.
    //The function atof does NOT support number strings with e or E.
    //These functions are reused for faster parsing string into numbers.

    static inline int atoi(const char *p, const char *&end) {
        assert(p);
        assert(*p != ' ');
        bool neg;
        if (*p == '-') {
            neg = true;
            ++p;
        } else {
            neg = false;
        }
        int x = 0;
        while (*p >= '0' && *p <= '9') {
            x = (x * 10) + (*p - '0');
            ++p;
        }
        if (neg) {
            x = -x;
        }
        end = p;
        return x;
    }

    static inline unsigned int atoui(const char *p, const char *&end) {
        assert(p);
        assert(*p != ' ');
        unsigned int x = 0;
        while (*p >= '0' && *p <= '9') {
            x = (x << 3) + (x << 1) + (*p - '0');
            ++p;
        }
        end = p;
        return x;
    }

    static inline INT64 atoll(const char *p, const char *&end) {
        assert(p);
        assert(*p != ' ');
        bool neg;
        if (*p == '-') {
            neg = true;
            ++p;
        } else {
            neg = false;
        }
        INT64 x = 0;
        while (*p >= '0' && *p <= '9') {
            x = (x * 10) + (*p - '0');
            ++p;
        }
        if (neg) {
            x = -x;
        }
        end = p;
        return x;
    }

    static inline UINT64 atoull(const char *p, const char *&end) {
        assert(p);
        assert(*p != ' ');
        UINT64 x = 0;
        while (*p >= '0' && *p <= '9') {
            x = (x << 3) + (x << 1) + (*p - '0');
            ++p;
        }
        end = p;
        return x;
    }

    static inline double atof(const char *p, const char *&end) {
        assert(p);
        assert(*p != ' ');
        bool neg;
        if (*p == '-') {
            neg = true;
            ++p;
        } else {
            neg = false;
        }
        double r = (double) atoull(p, end);
        if (*end == '.') {
            const char *start = ++end;
            double dot = (double) atoull(start, end);
            unsigned int n = (unsigned int) (end - start);
            UINT64 temp = 1;
            while (n) {
                temp = (temp << 3) + (temp << 1);
                --n;
            }
            dot /= temp;
            r += dot;
        }
        if (neg) {
            r = -r;
        }
        return r;
    }

#pragma pack(push,1)

    struct UDateTime {
    private:
        static const unsigned int MICRO_SECONDS = 0xfffff; //20 bits

    public:

        static void ParseTime(const char *str, int &hour, int &min, int &second, unsigned int &us) {
            const char *end;
            hour = atoi(str, end);
            min = atoi(++end, end);
            second = atoi(++end, end);
            if (*end == '.') {
                us = (unsigned int) (atof(end, end) * 1000000 + 0.5); //rounded to 1 microsecond
            } else {
                us = 0;
            }
        }

        static void ParseDateTime(const char *str, int &year, int &month, int &day, int &hour, int &min, int &second, unsigned int &us) {
            const char *end;
            year = atoi(str, end);
            month = atoi(++end, end);
            day = atoi(++end, end);
            if (*end == ' ') {
                ParseTime(++end, hour, min, second, us);
            } else {
                hour = 0;
                min = 0;
                second = 0;
                us = 0;
            }
        }

    public:
        UINT64 time; //in micro-second

        UDateTime(UINT64 t = 0) : time(t) {
        }

        UDateTime(const std::tm &dt, unsigned int us = 0) {
            Set(dt, us);
        }

        UDateTime(const char *str) {
            if (str) {
                ParseFromDBString(str);
            } else {
                time = 0;
            }
        }

        UDateTime& operator=(const UDateTime& dt) {
            if (this != &dt)
                time = dt.time;
            return *this;
        }

        inline bool operator==(const UDateTime & t) const {
            return (time == t.time);
        }

        inline bool operator!=(const UDateTime & t) const {
            return (time != t.time);
        }

        inline unsigned int HasMicrosecond() const {
            return (unsigned int) (time & MICRO_SECONDS);
        }

        inline unsigned int HasDate() const {
            return (unsigned int) (time >> 37);
        }

        inline unsigned int HasTime() const {
            return (unsigned int) ((time >> 20) & 0x1ffff);
        }

        //convert UDateTime datetime back to std::tm structure and micro-seconds

        std::tm GetCTime(unsigned int *us = nullptr) const {
            std::tm datetime;
            ::memset(&datetime, 0, sizeof (datetime));
            UINT64 dt = time;
            if (us) {
                *us = (unsigned int) (dt & MICRO_SECONDS);
            }
            dt >>= 20;
            datetime.tm_sec = (int) (dt & 0x3f);
            dt >>= 6;
            datetime.tm_min = (int) (dt & 0x3f);
            dt >>= 6;
            datetime.tm_hour = (int) (dt & 0x1f);
            dt >>= 5;
            datetime.tm_mday = (int) (dt & 0x1f);
            dt >>= 5;
            datetime.tm_mon = (int) (dt & 0xf);
            dt >>= 4;
            datetime.tm_year = (int) dt;
            return datetime;
        }

        void Set(const std::tm &dt, unsigned int us = 0) {
            assert(us < 1000000);
            if (us >= 1000000) {
                us = 999999;
            }
            assert(dt.tm_year >= 0);
            assert(dt.tm_mon >= 0 && dt.tm_mon <= 11);
            assert(dt.tm_mday >= 0 && dt.tm_mday < 32);
            assert(dt.tm_hour >= 0 && dt.tm_hour < 24);
            assert(dt.tm_min >= 0 && dt.tm_min < 60);
            assert(dt.tm_sec >= 0 && dt.tm_sec < 60);
            time = (unsigned int) (dt.tm_year & 0x3ffff);
            time <<= 46; //18 bits for years
            UINT64 mid = (unsigned int) (dt.tm_mon & 0xf); //4 bits for month
            mid <<= 42;
            time += mid;
            mid = (unsigned int) (dt.tm_mday & 0x1f); //5 bits for day
            mid <<= 37;
            time += mid;
            mid = (unsigned int) (dt.tm_hour & 0x1f); //5 bits for hour
            mid <<= 32;
            time += mid;
            mid = (unsigned int) (dt.tm_min & 0x3f); //6 bits for minute
            mid <<= 26;
            time += mid;
            mid = (unsigned int) (dt.tm_sec & 0x3f); //6 bits for second
            mid <<= 20;
            time += mid;
            time += (unsigned int) (us & MICRO_SECONDS); //20 bits for micro-seconds
        }

        void ToDBString(char *str, unsigned int bufferSize) const {
            unsigned int us;
            assert(str);
            assert(bufferSize > 26); //2012-12-23 23:59:59.123456
            std::tm tm = GetCTime(&us);
#if defined (WIN32_64) && _MSC_VER >= 1600 
            if (us) {
                if (tm.tm_mday) {
                    sprintf_s(str, bufferSize, "%04d-%02d-%02d %02d:%02d:%02d.%06d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, us);
                } else {
                    //time with micro-seconds
                    sprintf_s(str, bufferSize, "%02d:%02d:%02d.%06d", tm.tm_hour, tm.tm_min, tm.tm_sec, us);
                }
            } else if (tm.tm_hour || tm.tm_min || tm.tm_sec) {
                if (tm.tm_mday) {
                    sprintf_s(str, bufferSize, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
                } else {
                    //time without micro-seconds
                    sprintf_s(str, bufferSize, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
                }
            } else {
                sprintf_s(str, bufferSize, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            }
#else
            if (us) {
                if (tm.tm_mday) {
                    sprintf(str, "%04d-%02d-%02d %02d:%02d:%02d.%06d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, us);
                } else {
                    //time with micro-seconds
                    sprintf(str, "%02d:%02d:%02d.%06d", tm.tm_hour, tm.tm_min, tm.tm_sec, us);
                }
            } else if (tm.tm_hour || tm.tm_min || tm.tm_sec) {
                if (tm.tm_mday) {
                    sprintf(str, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
                } else {
                    //time without micro-seconds
                    sprintf(str, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
                }
            } else {
                sprintf(str, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            }
#endif
        }

        std::string ToDBString() const {
            char str[32] = {0};
            ToDBString(str, sizeof (str));
            return str;
        }

        void ToWebString(char *str, unsigned int bufferSize) const {
            unsigned int us;
            assert(str);
            assert(bufferSize > 24); //2012-12-23T23:59:59.123Z
            std::tm tm = GetCTime(&us);
            unsigned int ms = us / 1000;
#if defined (WIN32_64) && _MSC_VER >= 1600 
            if (ms) {
                if (tm.tm_mday) {
                    sprintf_s(str, bufferSize, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
                } else {
                    //time with micro-seconds
                    sprintf_s(str, bufferSize, "%02d:%02d:%02d.%03dZ", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
                }
            } else if (tm.tm_hour || tm.tm_min || tm.tm_sec) {
                if (tm.tm_mday) {
                    sprintf_s(str, bufferSize, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
                } else {
                    //time without micro-seconds
                    sprintf_s(str, bufferSize, "%02d:%02d:%02dZ", tm.tm_hour, tm.tm_min, tm.tm_sec);
                }
            } else {
                sprintf_s(str, bufferSize, "%04d-%02d-%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            }
#else
            if (ms) {
                if (tm.tm_mday) {
                    sprintf(str, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
                } else {
                    //time with micro-seconds
                    sprintf(str, "%02d:%02d:%02d.%03dZ", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
                }
            } else if (tm.tm_hour || tm.tm_min || tm.tm_sec) {
                if (tm.tm_mday) {
                    sprintf(str, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
                } else {
                    //time without micro-seconds
                    sprintf(str, "%02d:%02d:%02dZ", tm.tm_hour, tm.tm_min, tm.tm_sec);
                }
            } else {
                sprintf(str, "%04d-%02d-%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            }
#endif
        }

        std::string ToWebString() const {
            char str[32] = {0};
            ToWebString(str, sizeof (str));
            return str;
        }

        //string must be in the form like '2016-02-15 22:03:14.234 or 22:03:14.234

        void ParseFromDBString(const char *str) {
            assert(str);
            unsigned int us = 0;
            std::tm tm;
            ::memset(&tm, 0, sizeof (tm));
            const char *pos = strchr(str, ':');
            const char *whitespace = strchr(str, ' ');
            if (!whitespace && pos) {
                ParseTime(str, tm.tm_hour, tm.tm_min, tm.tm_sec, us);
            } else {
                ParseDateTime(str, tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, us);
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
            }
            Set(tm, us);
        }

#ifdef WIN32_64

        UDateTime(const SYSTEMTIME &dt, unsigned short us = 0) {
            Set(dt, us);
        }

        //vtDate -- windows variant datetime with accuracy to millisecond

        UDateTime(double vtDate, unsigned short us = 0) {
            Set(vtDate, us);
        }

        void Set(const SYSTEMTIME &dt, unsigned short us = 0) {
            //micro-second must be less than 1000
            assert(us < 1000);
            if (us >= 1000) {
                us = 999;
            }
            assert(dt.wYear >= 1900);
            assert(dt.wMonth > 0 && dt.wMonth <= 12);
            assert(dt.wDay > 0 && dt.wDay < 32);
            assert(dt.wHour >= 0 && dt.wHour < 24);
            assert(dt.wMinute >= 0 && dt.wMinute < 60);
            assert(dt.wSecond >= 0 && dt.wSecond < 60);
            time = (unsigned int) ((dt.wYear - 1900) & 0x3ffff);
            time <<= 46; //18 bits for years
            UINT64 mid = (unsigned int) ((dt.wMonth - 1) & 0xf); //4 bits for month
            mid <<= 42;
            time += mid;
            mid = (unsigned int) (dt.wDay & 0x1f); //5 bits for day
            mid <<= 37;
            time += mid;
            mid = (unsigned int) (dt.wHour & 0x1f); //5 bits for hour
            mid <<= 32;
            time += mid;
            mid = (unsigned int) (dt.wMinute & 0x3f); //6 bits for minute
            mid <<= 26;
            time += mid;
            mid = (unsigned int) (dt.wSecond & 0x3f); //6 bits for second
            mid <<= 20;
            time += mid;
            unsigned int micro_second = (unsigned int) dt.wMilliseconds * 1000;
            micro_second += us;
            time += micro_second;
        }

        void Set(double vtDate, unsigned short us = 0) {
            SYSTEMTIME st;
            VariantTimeToSystemTime(vtDate, &st);
            st.wMilliseconds = (WORD) (ToEpoch(vtDate) % 1000);
            Set(st, us);
        }

        SYSTEMTIME GetSysTime(unsigned short *microseconds = nullptr) const {
            SYSTEMTIME datetime;
            ::memset(&datetime, 0, sizeof (datetime));
            UINT64 dt = (UINT64) time;
            unsigned int micro_seconds = (unsigned int) (dt & 0xfffff);
            datetime.wMilliseconds = (WORD) (micro_seconds / 1000);
            if (microseconds) {
                *microseconds = (unsigned short) (micro_seconds % 1000);
            }
            dt >>= 20;
            datetime.wSecond = (WORD) (dt & 0x3f);
            dt >>= 6;
            datetime.wMinute = (WORD) (dt & 0x3f);
            dt >>= 6;
            datetime.wHour = (WORD) (dt & 0x1f);
            dt >>= 5;
            datetime.wDay = (WORD) (dt & 0x1f);
            dt >>= 5;
            datetime.wMonth = (WORD) (dt & 0xf) + 1;
            dt >>= 4;
            datetime.wYear = (WORD) (dt + 1900);
            return datetime;
        }

        //windows variant datetime with accuracy to millisecond

        double GetVariantDate(unsigned short *microseconds = nullptr) const {
            unsigned short us;
            SYSTEMTIME st = GetSysTime(&us);
            double vtDate;
            SystemTimeToVariantTime(&st, &vtDate);
            double ms = st.wMilliseconds;
            vtDate += ms / MILLISECONDS_PER_DAY;
            if (microseconds) {
                *microseconds = us;
            }
            return vtDate;
        }
#else

        void Set(const timeval &tv) {
            std::tm *tm = std::localtime(&tv.tv_sec);
            unsigned int us = tv.tv_usec;
            Set(*tm, us);
        }

        UDateTime(const timeval &tv) {
            Set(tv);
        }
#endif
    };
#pragma pack(pop) //ensure sizeof(UDateTime) == 8
#if defined(WINCE) || defined(UNDER_CE) || defined(_WIN32_WCE)

#else
    static_assert(sizeof (UDateTime) == 8, "Wrong UDateTime size");
#endif
};

#endif //__UCOMM_BASE_DEFINES_H_____
