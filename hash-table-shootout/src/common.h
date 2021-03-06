#pragma once

#include <functional>

using KeyType = int64_t;
using ValueType = int64_t;
using KeyEq = std::equal_to<KeyType>;

#if defined(MURMUR3)
    //===------------------------------------------------------------------===//
    // MURMUR hashing
    //===------------------------------------------------------------------===//
    #include "MurmurHash3.h"    
    class Murmur3Hasher {
     public:
        template <typename T>
        uint64_t operator()(const T &value) const {
            uint32_t hash = 0;
            auto *buf = reinterpret_cast<const char *>(&value);
            MurmurHash3_x86_32(buf, sizeof(T), 123445, &hash);
            return hash;
        } 
        
        /*template <typename T>
        uint64_t operator()(T &value) const {
            return (*this)(static_cast<const T &>(value));
        }*/
    };
    using Hasher = Murmur3Hasher;

    class Murmur3StringHasher {
     public:
        template <typename T>
        uint64_t operator()(const T &value) const {
            uint32_t hash = 0;
            const char *buffer = value.c_str();
            MurmurHash3_x86_32(buffer, value.length(), 123445, &hash);
            return hash;
        }
    };
    using StringHasher = Murmur3StringHasher;



#elif defined(CLHASH)
    //===------------------------------------------------------------------===//
    /// CLHash hashing
    //===------------------------------------------------------------------===//
    #include "clhash.h"
    class CLHashHasher {
    private:
        void *random;

    public:
        CLHashHasher() {
            random = get_random_key_for_clhash(UINT64_C(0x23a23cf5033c3c81),
                                               UINT64_C(0xb3816f6a2c68e530));
        }

        template <typename T>
        inline uint64_t operator()(const T &value) const {
            const char *buffer = reinterpret_cast<const char *>(&value);
            return clhash(random, buffer, sizeof(value));
        }
    };
    using Hasher = CLHashHasher;

    class CLHashStringHasher {
    private:
        void *random;

    public:
        CLHashStringHasher() {
            random = get_random_key_for_clhash(UINT64_C(0x23a23cf5033c3c81),
                                               UINT64_C(0xb3816f6a2c68e530));
        }

        template <typename T>
        inline uint64_t operator()(const T &value) const {
            const char *buffer = value.c_str();
            return clhash(random, buffer, value.length());
        }
    };
    using StringHasher = CLHashStringHasher;

#elif defined(XXHASH)
    //===------------------------------------------------------------------===//
    /// xxHash hashing
    //===------------------------------------------------------------------===//
    #define XXH_PRIVATE_API
    #include "xxhash.h"
    #undef XXH_PRIVATE_API
    class xxHashHasher {
    public:
        template <typename T>
        inline uint64_t operator()(const T &value) const {
            unsigned long long const seed = 0;
            const char *buffer = reinterpret_cast<const char *>(&value);
            return XXH64(buffer, sizeof(T), seed);
        }
    };
    using Hasher = xxHashHasher;

    class xxHashStringHasher {
    public:
        template <typename T>
        inline uint64_t operator()(const T &value) const {
            unsigned long long const seed = 0;
            const char *buffer = value.c_str();
            return XXH64(buffer, value.length(), seed);
        }
    };
    using StringHasher = xxHashStringHasher;

#elif defined(FARMHASH)
    //===------------------------------------------------------------------===//
    /// FarmHash hashing
    //===------------------------------------------------------------------===//
    #include "farmhash.h"
    class FarmHashHasher {
    public:
        template <typename T>
        inline uint64_t operator()(const T &value) const {
            const char *buffer = reinterpret_cast<const char *>(&value);
            return util::Hash32(buffer, sizeof(T));
        }
    };
    using Hasher = FarmHashHasher;

    class FarmHashStringHasher {
    public:
        template <typename T>
        inline uint64_t operator()(const T &value) const {
            const char *buffer = value.c_str();
            return util::Hash32(buffer, value.length());
        }
    };
    using StringHasher = FarmHashStringHasher;


#elif defined(MULTHASH)
    //===------------------------------------------------------------------===//
    /// Multiplicative hashing
    //===------------------------------------------------------------------===//
    #error "We don't support Multiplicative hashing yet"
    class MultiplicativeHasher {
     public:
      static uint64_t shift;  
      static uint64_t factor;

      inline uint64_t operator()(uint64_t value) const {
        return (value * factor) >> shift;
      }
    };

#elif defined(CRCHASH)
    //===------------------------------------------------------------------===//
    // CRC Hashing
    //===------------------------------------------------------------------===//
    #include <immintrin.h>
    class CRC_HW {
    public:
        template <typename T>
        uint64_t operator()(const T &value) const; 

        template <typename T>
        uint64_t operator()(T &value) const {
            return (*this)(static_cast<const T &>(value));
        }
    };

    #define CRC32(op, crc, type, buf, len)                   \
      do {                                                   \
        for (; (len) >= sizeof(type);                        \
             (len) -= sizeof(type), (buf) += sizeof(type)) { \
          (crc) = op((crc), *(type *)buf);                   \
        }                                                    \
      } while (0)

    template <typename T>
    inline uint64_t CRC_HW::operator()(const T &value) const {
        uint64_t crc = 0;
        uint64_t length = sizeof(T);
        const char *buf = reinterpret_cast<const char *>(&value);
    #if defined(__x86_64__) || defined(_M_X64)
        // Try to each up as many 8-byte values as possible
        CRC32(_mm_crc32_u64, crc, uint64_t, buf, length);
    #endif
        // Now we perform CRC in 4-byte, then 2-byte, then byte chunks
        CRC32(_mm_crc32_u32, crc, uint32_t, buf, length);
        CRC32(_mm_crc32_u16, crc, uint16_t, buf, length);
        CRC32(_mm_crc32_u8, crc, uint8_t, buf, length);
        // Done
        return crc;
    } 

    template <>
    inline uint64_t CRC_HW::operator()<int64_t>(const int64_t &value) const {
        return _mm_crc32_u32(0x4c11db7, value);
    }

    using Hasher = CRC_HW;

    class CRC_HW_STRING {
    public:
        template <typename T>
        uint64_t operator()(const T &value) const;

        template <typename T>
        uint64_t operator()(T &value) const {
            return (*this)(static_cast<const T &>(value));
        }
    };

    template <typename T>
    inline uint64_t CRC_HW_STRING::operator()(const T &value) const {
        uint64_t crc = 0;
        uint64_t length = value.length();
        const char *buf = value.c_str();
    #if defined(__x86_64__) || defined(_M_X64)
        // Try to each up as many 8-byte values as possible
        CRC32(_mm_crc32_u64, crc, uint64_t, buf, length);
    #endif
        // Now we perform CRC in 4-byte, then 2-byte, then byte chunks
        CRC32(_mm_crc32_u32, crc, uint32_t, buf, length);
        CRC32(_mm_crc32_u16, crc, uint16_t, buf, length);
        CRC32(_mm_crc32_u8, crc, uint8_t, buf, length);
        // Done
        return crc;
    }
    template <>
    inline uint64_t CRC_HW_STRING::operator()<std::string>(const std::string &value) const {
        uint64_t crc = 0;
        uint64_t length = value.length();
        const char *buf = value.c_str();
    #if defined(__x86_64__) || defined(_M_X64)
        // Try to each up as many 8-byte values as possible
        CRC32(_mm_crc32_u64, crc, uint64_t, buf, length);
    #endif
        // Now we perform CRC in 4-byte, then 2-byte, then byte chunks
        CRC32(_mm_crc32_u32, crc, uint32_t, buf, length);
        CRC32(_mm_crc32_u16, crc, uint16_t, buf, length);
        CRC32(_mm_crc32_u8, crc, uint8_t, buf, length);
        // Done
        return crc;
    }

    #undef CRC32
    using StringHasher = CRC_HW_STRING;

#elif defined(SIMPLEHASH)
    //===----------------------------------------------------------------------===//
    /// Simple XOR hashing
    //===----------------------------------------------------------------------===//
    class SimpleInt64Hasher {
     public:  
      inline uint64_t operator()(uint64_t value) const {
        return (value >> 7) ^ (value >> 13 ) ^ (value >> 21 ) ^ value;
      }
    };
    using Hasher = SimpleInt64Hasher;

    class SimpleStringHasher {
     public:
      inline uint64_t operator()(std::string value) const {
        uint64_t hash = 5381;
        int c;
        const char *buf = value.c_str();
        while (c = *buf++)
          hash = ((hash << 5) + hash) + c;
        return hash;
      }
    };
    using StringHasher = SimpleStringHasher;

#else
    //===------------------------------------------------------------------===//
    // STD Hash
    //===------------------------------------------------------------------===//
    using Hasher = std::hash<KeyType>;
    using StringHasher = std::hash<std::string>;
#endif

//===----------------------------------------------------------------------===//
// This is an object that represents a fixed length object
//===----------------------------------------------------------------------===//
template <size_t sz>
class FixedLenValue {
 public:
  char data[sz];
};
