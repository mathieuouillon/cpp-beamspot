#pragma once
#include <cstdint>

namespace hipo {

// --- Magic numbers ---
inline constexpr uint32_t HIPO_FILE_UNIQUE_WORD = 0x4F504948; // "HIPO" in LE
inline constexpr uint32_t HEADER_MAGIC          = 0xc0da0100; // endianness marker
inline constexpr uint32_t HEADER_MAGIC_BE       = 0x0001dac0; // big-endian marker

// --- Header sizes (in 32-bit words) ---
inline constexpr int FILE_HEADER_WORDS   = 14;
inline constexpr int RECORD_HEADER_WORDS = 14;
inline constexpr int FILE_HEADER_SIZE    = FILE_HEADER_WORDS * 4;     // 56 bytes
inline constexpr int RECORD_HEADER_SIZE  = RECORD_HEADER_WORDS * 4;   // 56 bytes
inline constexpr int EVENT_HEADER_SIZE   = 16;
inline constexpr int BANK_STRUCTURE_SIZE = 8;

// --- File header field offsets (byte offsets) ---
inline constexpr int FH_UNIQUE_WORD_OFFSET     = 0;   // word 1
inline constexpr int FH_FILE_NUMBER_OFFSET     = 4;   // word 2
inline constexpr int FH_HEADER_LENGTH_OFFSET   = 8;   // word 3 (in words)
inline constexpr int FH_RECORD_COUNT_OFFSET    = 12;  // word 4
inline constexpr int FH_INDEX_ARRAY_LEN_OFFSET = 16;  // word 5 (in bytes)
inline constexpr int FH_BIT_INFO_OFFSET        = 20;  // word 6
inline constexpr int FH_USER_HEADER_LEN_OFFSET = 24;  // word 7 (in bytes)
inline constexpr int FH_MAGIC_NUMBER_OFFSET    = 28;  // word 8
inline constexpr int FH_USER_REGISTER_OFFSET   = 32;  // words 9-10 (64-bit)
inline constexpr int FH_TRAILER_POS_OFFSET     = 40;  // words 11-12 (64-bit)
inline constexpr int FH_USER_INT1_OFFSET       = 48;  // word 13
inline constexpr int FH_USER_INT2_OFFSET       = 52;  // word 14

// --- Record header field offsets (byte offsets) ---
inline constexpr int RH_RECORD_LENGTH_OFFSET   = 0;   // word 1
inline constexpr int RH_RECORD_NUMBER_OFFSET   = 4;   // word 2
inline constexpr int RH_HEADER_LENGTH_OFFSET   = 8;   // word 3 (in words)
inline constexpr int RH_EVENT_COUNT_OFFSET     = 12;  // word 4
inline constexpr int RH_INDEX_ARRAY_LEN_OFFSET = 16;  // word 5 (in bytes)
inline constexpr int RH_BIT_INFO_OFFSET        = 20;  // word 6
inline constexpr int RH_USER_HEADER_LEN_OFFSET = 24;  // word 7
inline constexpr int RH_MAGIC_NUMBER_OFFSET    = 28;  // word 8
inline constexpr int RH_DATA_LENGTH_OFFSET     = 32;  // word 9
inline constexpr int RH_COMP_WORD_OFFSET       = 36;  // word 10
inline constexpr int RH_USER_WORD1_OFFSET      = 40;  // words 11-12 (64-bit)
inline constexpr int RH_USER_WORD2_OFFSET      = 48;  // words 13-14 (64-bit)

// --- Event header field offsets (byte offsets) ---
inline constexpr int EH_MAGIC_OFFSET    = 0;
inline constexpr int EH_SIZE_OFFSET     = 4;
inline constexpr int EH_TAG_OFFSET      = 8;
inline constexpr int EH_RESERVED_OFFSET = 12;

// --- Dictionary identifiers ---
inline constexpr int DICT_GROUP        = 120;
inline constexpr int DICT_ITEM         = 2;
inline constexpr int DICT_JSON_ITEM    = 1;
inline constexpr int CONFIG_GROUP      = 32555;
inline constexpr int CONFIG_KEY_ITEM   = 1;
inline constexpr int CONFIG_STRING_ITEM = 2;
inline constexpr int FILE_INDEX_GROUP  = 32111;
inline constexpr int FILE_INDEX_ITEM   = 1;

// --- Header type (bits[28-31] of bit-info in file header) ---
enum class header_type : uint8_t {
    evio_record      = 0,
    evio_file        = 1,
    evio_ext_file    = 2,
    hipo_record      = 4,
    hipo_file        = 5,
    hipo_ext_file    = 6,
    hipo_trailer     = 7,
};

// --- Compression type (bits[28-31] of compression word) ---
enum class compression_type : uint8_t {
    none     = 0,
    lz4      = 1,
    lz4_best = 2,
    gzip     = 3,
};

// --- Bit-info word layout ---
// File header bit-info:
//   bits[0-7]   = version
//   bit[8]      = has dictionary (first record)
//   bit[9]      = has "first event" (in every split file)
//   bit[10]     = file trailer with index exists
//   bits[11-19] = reserved
//   bits[20-21] = pad1 (user header padding)
//   bits[22-23] = pad2 (data padding)
//   bits[24-25] = pad3 (compressed data padding)
//   bits[26-27] = reserved
//   bits[28-31] = general header type
//
// Record header bit-info:
//   bits[0-7]   = version
//   bit[8]      = last record
//   bit[9]      = reserved
//   bit[10]     = has-dictionary (record has dictionary in user header)
//   bit[11]     = has-first-event
//   bits[20-21] = pad1
//   bits[22-23] = pad2
//   bits[24-25] = pad3
inline constexpr uint32_t BITINFO_VERSION_MASK          = 0x000000FF;
inline constexpr int      BITINFO_VERSION_BITS          = 8;
inline constexpr int      BITINFO_HAS_DICTIONARY_BIT    = 8;
inline constexpr int      BITINFO_HAS_FIRST_EVENT_BIT   = 9;
inline constexpr int      BITINFO_TRAILER_WITH_INDEX_BIT = 10;
inline constexpr int      BITINFO_PAD1_SHIFT            = 20;
inline constexpr int      BITINFO_PAD2_SHIFT            = 22;
inline constexpr int      BITINFO_PAD3_SHIFT            = 24;
inline constexpr uint32_t BITINFO_PAD_MASK              = 0x3;
inline constexpr int      BITINFO_HEADER_TYPE_SHIFT     = 28;

// --- Compression word layout ---
// bits[28-31] = compression type
// bits[0-27]  = compressed data length in 32-bit WORDS (not bytes!)
inline constexpr uint32_t COMP_TYPE_MASK   = 0xF0000000;
inline constexpr int      COMP_TYPE_SHIFT  = 28;
inline constexpr uint32_t COMP_TYPE_BYTE   = 0x0000000F; // after shift
inline constexpr uint32_t COMP_LENGTH_MASK = 0x0FFFFFFF;

// --- Bank/node structure word layout ---
// The 32-bit size word at offset +4 in a structure header:
//   bits[0-23]  = total size (format + data)
//   bits[24-31] = format descriptor length
inline constexpr uint32_t STRUCT_SIZE_MASK    = 0x00FFFFFF;
inline constexpr uint32_t STRUCT_FORMAT_MASK  = 0xFF000000;
inline constexpr int      STRUCT_FORMAT_SHIFT = 24;
inline constexpr uint32_t STRUCT_FORMAT_BYTE  = 0x000000FF; // after shift

// --- Protocol version ---
inline constexpr int HIPO_VERSION = 6;

} // namespace hipo
