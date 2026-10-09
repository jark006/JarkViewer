// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef TYPES_HPP_
#define TYPES_HPP_

#include "exiv2lib_export.h"

// included header files
#include "config.h"
#include "slice.hpp"

// standard includes
#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <vector>

// *****************************************************************************
// namespace extensions
namespace Exiv2 {
// *****************************************************************************
// type definitions

//! 1 byte unsigned integer type.
using byte = uint8_t;

//! 8 byte unsigned rational type.
using URational = std::pair<uint32_t, uint32_t>;
//! 8 byte signed rational type.
using Rational = std::pair<int32_t, int32_t>;

//! Type to express the byte order (little or big endian)
enum ByteOrder {
  invalidByteOrder,
  littleEndian,
  bigEndian,
};

//! Type to indicate write method used by TIFF parsers
enum WriteMethod {
  wmIntrusive,
  wmNonIntrusive,
};

//! An identifier for each type of metadata
enum MetadataId {
  mdNone = 0,
  mdExif = 1,
  mdIptc = 2,
  mdComment = 4,
  mdXmp = 8,
  mdIccProfile = 16,
};

//! An identifier for each mode of metadata support
enum AccessMode {
  amNone = 0,
  amRead = 1,
  amWrite = 2,
  amReadWrite = 3,
};

/*!
  @brief %Exiv2 value type identifiers.

  Used primarily as identifiers when creating %Exiv2 Value instances.
  See Value::create. 0x0000 to 0xffff are reserved for TIFF (Exif) types.
 */
enum TypeId {
  unsignedByte = 1,         //!< Exif BYTE type, 8-bit unsigned integer.
  asciiString = 2,          //!< Exif ASCII type, 8-bit byte.
  unsignedShort = 3,        //!< Exif SHORT type, 16-bit (2-byte) unsigned integer.
  unsignedLong = 4,         //!< Exif LONG type, 32-bit (4-byte) unsigned integer.
  unsignedRational = 5,     //!< Exif RATIONAL type, two LONGs: numerator and denominator of a fraction.
  signedByte = 6,           //!< Exif SBYTE type, an 8-bit signed (twos-complement) integer.
  undefined = 7,            //!< Exif UNDEFINED type, an 8-bit byte that may contain anything.
  signedShort = 8,          //!< Exif SSHORT type, a 16-bit (2-byte) signed (twos-complement) integer.
  signedLong = 9,           //!< Exif SLONG type, a 32-bit (4-byte) signed (twos-complement) integer.
  signedRational = 10,      //!< Exif SRATIONAL type, two SLONGs: numerator and denominator of a fraction.
  tiffFloat = 11,           //!< TIFF FLOAT type, single precision (4-byte) IEEE format.
  tiffDouble = 12,          //!< TIFF DOUBLE type, double precision (8-byte) IEEE format.
  tiffIfd = 13,             //!< TIFF IFD type, 32-bit (4-byte) unsigned integer.
  unsignedLongLong = 16,    //!< Exif LONG LONG type, 64-bit (8-byte) unsigned integer.
  signedLongLong = 17,      //!< Exif LONG LONG type, 64-bit (8-byte) signed integer.
  tiffIfd8 = 18,            //!< TIFF IFD type, 64-bit (8-byte) unsigned integer.
  string = 0x10000,         //!< IPTC string type.
  date = 0x10001,           //!< IPTC date type.
  time = 0x10002,           //!< IPTC time type.
  comment = 0x10003,        //!< %Exiv2 type for the Exif user comment.
  directory = 0x10004,      //!< %Exiv2 type for a CIFF directory.
  xmpText = 0x10005,        //!< XMP text type.
  xmpAlt = 0x10006,         //!< XMP alternative type.
  xmpBag = 0x10007,         //!< XMP bag type.
  xmpSeq = 0x10008,         //!< XMP sequence type.
  langAlt = 0x10009,        //!< XMP language alternative type.
  invalidTypeId = 0x1fffe,  //!< Invalid type id.
  lastTypeId = 0x1ffff,     //!< Last type id.
};

//! Container for binary data
using Blob = std::vector<byte>;

// *****************************************************************************
// class definitions

//! Type information lookup functions. Implemented as a static class.
class EXIV2API TypeInfo {
 public:
  //! Return the name of the type, 0 if unknown.
  static const char* typeName(TypeId typeId);
  //! Return the type id for a type name
  static TypeId typeId(const std::string& typeName);
  //! Return the size in bytes of one element of this type
  static size_t typeSize(TypeId typeId);
};

/*!
  @brief Utility class containing a character array. All it does is to take
         care of memory allocation and deletion. Its primary use is meant to
         be as a stack variable in functions that need a temporary data
         buffer.
 */
struct EXIV2API DataBuf {
  //! @name Creators
  //@{
  //! Default constructor
  DataBuf() = default;
  //! Constructor with an initial buffer size
  explicit DataBuf(size_t size);
  //! Constructor, copies an existing buffer
  DataBuf(const byte* pData, size_t size);
  //@}

  //! @name Manipulators
  //@{
  /*!
    @brief Allocate a data buffer of at least the given size. Note that if
           the requested \em size is less than the current buffer size, no
           new memory is allocated and the buffer size doesn't change.
   */
  void alloc(size_t size);
  /*!
    @brief Resize the buffer. Existing data is preserved (like std::realloc()).
   */
  void resize(size_t size);

  //! Reset value
  void reset();
  //@}

  using iterator = std::vector<byte>::iterator;
  using const_iterator = std::vector<byte>::const_iterator;

  iterator begin() noexcept {
    return pData_.begin();
  }
  [[nodiscard]] const_iterator cbegin() const noexcept {
    return pData_.cbegin();
  }
  iterator end() noexcept {
    return pData_.end();
  }
  [[nodiscard]] const_iterator cend() const noexcept {
    return pData_.end();
  }

  [[nodiscard]] size_t size() const {
    return pData_.size();
  }

  [[nodiscard]] uint8_t read_uint8(size_t offset) const;
  void write_uint8(size_t offset, uint8_t x);

  [[nodiscard]] uint16_t read_uint16(size_t offset, ByteOrder byteOrder) const;
  void write_uint16(size_t offset, uint16_t x, ByteOrder byteOrder);

  [[nodiscard]] uint32_t read_uint32(size_t offset, ByteOrder byteOrder) const;
  void write_uint32(size_t offset, uint32_t x, ByteOrder byteOrder);

  [[nodiscard]] uint64_t read_uint64(size_t offset, ByteOrder byteOrder) const;
  void write_uint64(size_t offset, uint64_t x, ByteOrder byteOrder);

  //! Equivalent to: memcmp(&pData_[offset], buf, bufsize)
  int cmpBytes(size_t offset, const void* buf, size_t bufsize) const;

  //! Returns a data pointer.
  [[nodiscard]] byte* data(size_t offset = 0);

  //! Returns a (read-only) data pointer.
  [[nodiscard]] const byte* c_data(size_t offset = 0) const;

  //! Returns a (read-only) C-style string pointer.
  [[nodiscard]] const char* c_str(size_t offset = 0) const;

  [[nodiscard]] bool empty() const {
    return pData_.empty();
  }

 private:
  Blob pData_;
};

/*!
 * @brief Create a new Slice from a DataBuf given the bounds.
 *
 * @param[in] buf  The DataBuf from which' data the Slice will be
 *     constructed
 * @param[in] begin Beginning bound of the new Slice. Must be smaller
 *     than `end` and both must not be larger than LONG_MAX.
 * @param[in] end End bound of the new Slice. Must be smaller
 *     than `end` and both must not be larger than LONG_MAX.
 *
 * @throw std::invalid_argument when `end` is larger than `LONG_MAX` or
 * anything that the constructor of @ref Slice throws
 */
EXIV2API Slice<byte*> makeSlice(DataBuf& buf, size_t begin, size_t end);

//! Overload of makeSlice for `const DataBuf`, returning an immutable Slice
EXIV2API Slice<const byte*> makeSlice(const DataBuf& buf, size_t begin, size_t end);

// *****************************************************************************
// free functions

//! Read a 2 byte unsigned short value from the data buffer
EXIV2API uint16_t getUShort(const byte* buf, ByteOrder byteOrder);
//! Read a 2 byte unsigned short value from a Slice
template <typename T>
uint16_t getUShort(const Slice<T>& buf, ByteOrder byteOrder) {
  if (byteOrder == littleEndian) {
    return static_cast<byte>(buf.at(1)) << 8 | static_cast<byte>(buf.at(0));
  }
  return static_cast<byte>(buf.at(0)) << 8 | static_cast<byte>(buf.at(1));
}

//! Read a 4 byte unsigned long value from the data buffer
EXIV2API uint32_t getULong(const byte* buf, ByteOrder byteOrder);
//! Read a 8 byte unsigned long value from the data buffer
EXIV2API uint64_t getULongLong(const byte* buf, ByteOrder byteOrder);
//! Read an 8 byte unsigned rational value from the data buffer
EXIV2API URational getURational(const byte* buf, ByteOrder byteOrder);
//! Read a 2 byte signed short value from the data buffer
EXIV2API int16_t getShort(const byte* buf, ByteOrder byteOrder);
//! Read a 4 byte signed long value from the data buffer
EXIV2API int32_t getLong(const byte* buf, ByteOrder byteOrder);
//! Read an 8 byte signed rational value from the data buffer
EXIV2API Rational getRational(const byte* buf, ByteOrder byteOrder);
//! Read a 4 byte single precision floating point value (IEEE 754 binary32) from the data buffer
EXIV2API float getFloat(const byte* buf, ByteOrder byteOrder);
//! Read an 8 byte double precision floating point value (IEEE 754 binary64) from the data buffer
EXIV2API double getDouble(const byte* buf, ByteOrder byteOrder);

//! Output operator for our fake rational
EXIV2API std::ostream& operator<<(std::ostream& os, const Rational& r);
//! Input operator for our fake rational
EXIV2API std::istream& operator>>(std::istream& is, Rational& r);
//! Output operator for our fake unsigned rational
EXIV2API std::ostream& operator<<(std::ostream& os, const URational& r);
//! Input operator for our fake unsigned rational
EXIV2API std::istream& operator>>(std::istream& is, URational& r);

/*!
  @brief Convert an unsigned short to data, write the data to the buffer,
         return number of bytes written.
 */
EXIV2API size_t us2Data(byte* buf, uint16_t s, ByteOrder byteOrder);
/*!
  @brief Convert an unsigned long to data, write the data to the buffer,
         return number of bytes written.
 */
EXIV2API size_t ul2Data(byte* buf, uint32_t l, ByteOrder byteOrder);
/*!
  @brief Convert an uint64_t to data, write the data to the buffer,
         return number of bytes written.
 */
EXIV2API size_t ull2Data(byte* buf, uint64_t l, ByteOrder byteOrder);
/*!
  @brief Convert an unsigned rational to data, write the data to the buffer,
         return number of bytes written.
 */
EXIV2API size_t ur2Data(byte* buf, URational l, ByteOrder byteOrder);
/*!
  @brief Convert a signed short to data, write the data to the buffer,
         return number of bytes written.
 */
EXIV2API size_t s2Data(byte* buf, int16_t s, ByteOrder byteOrder);
/*!
  @brief Convert a signed long to data, write the data to the buffer,
         return number of bytes written.
 */
EXIV2API size_t l2Data(byte* buf, int32_t l, ByteOrder byteOrder);
/*!
  @brief Convert a signed rational to data, write the data to the buffer,
         return number of bytes written.
 */
EXIV2API size_t r2Data(byte* buf, Rational l, ByteOrder byteOrder);
/*!
  @brief Convert a single precision floating point (IEEE 754 binary32) float
         to data, write the data to the buffer, return number of bytes written.
 */
EXIV2API size_t f2Data(byte* buf, float f, ByteOrder byteOrder);
/*!
  @brief Convert a double precision floating point (IEEE 754 binary64) double
         to data, write the data to the buffer, return number of bytes written.
 */
EXIV2API size_t d2Data(byte* buf, double d, ByteOrder byteOrder);

/*!
  @brief Print len bytes from buf in hex and ASCII format to the given
         stream, prefixed with the position in the buffer adjusted by
         offset.
 */
EXIV2API void hexdump(std::ostream& os, const byte* buf, size_t len, size_t offset = 0);

/*!
  @brief Return true if str is a hex number starting with prefix followed
         by size hex digits, false otherwise. If size is 0, any number of
         digits is allowed and all are checked.
 */
EXIV2API bool isHex(const std::string& str, size_t size = 0, const std::string& prefix = "");

/*!
  @brief Converts a string in the form "%Y:%m:%d %H:%M:%S", e.g.,
         "2007:05:24 12:31:55" to broken down time format,
         returns 0 if successful, else 1.
 */
EXIV2API int exifTime(const char* buf, tm* tm);

/*!
  @brief Translate a string using the gettext framework. This wrapper hides
         all the implementation details from the interface.
 */
EXIV2API const char* exvGettext(const char* str);

/*!
  @brief Return a \em int64_t set to the value represented by \em s.

  Besides strings that represent \em int64_t values, the function also
  handles \em float, \em Rational and boolean
  (see also: stringTo(const std::string& s, bool& ok)).

  @param  s  String to parse
  @param  ok Output variable indicating the success of the operation.
  @return Returns the \em int64_t value represented by \em s and sets \em ok
          to \c true if the conversion was successful or \c false if not.
*/
EXIV2API int64_t parseInt64(const std::string& s, bool& ok);

/*!
  @brief Return a \em uint32_t set to the value represented by \em s.

  Besides strings that represent \em uint32_t values, the function also
  handles \em float, \em Rational and boolean
  (see also: stringTo(const std::string& s, bool& ok)).

  @param  s  String to parse
  @param  ok Output variable indicating the success of the operation.
  @return Returns the \em uint32_t value represented by \em s and sets \em ok
          to \c true if the conversion was successful or \c false if not.
*/
EXIV2API uint32_t parseUint32(const std::string& s, bool& ok);

/*!
  @brief Return a \em float set to the value represented by \em s.

  Besides strings that represent \em float values, the function also
  handles \em long, \em Rational and boolean
  (see also: stringTo(const std::string& s, bool& ok)).

  @param  s  String to parse
  @param  ok Output variable indicating the success of the operation.
  @return Returns the \em float value represented by \em s and sets \em ok
          to \c true if the conversion was successful or \c false if not.
*/
EXIV2API float parseFloat(const std::string& s, bool& ok);

/*!
  @brief Return a \em Rational set to the value represented by \em s.

  Besides strings that represent \em Rational values, the function also
  handles \em long, \em float and boolean
  (see also: stringTo(const std::string& s, bool& ok)).
  Uses floatToRationalCast(float f) if the string can be parsed into a
  \em float.

  @param  s  String to parse
  @param  ok Output variable indicating the success of the operation.
  @return Returns the \em Rational value represented by \em s and sets \em ok
          to \c true if the conversion was successful or \c false if not.
*/
EXIV2API Rational parseRational(const std::string& s, bool& ok);

/*!
  @brief Very simple conversion of a \em float to a \em Rational.

  Test it with the values that you expect and check the implementation
  to see if this is really what you want!
 */
EXIV2API Rational floatToRationalCast(float f);

// *****************************************************************************
// template and inline definitions

/*!
  @brief Find an element that matches \em key in the array \em src.

  Designed to be used with lookup tables as shown in the example below.
  Requires a %Key structure (ideally in the array) and a comparison operator
  to compare a key with an array element.  The size of the array is
  determined automagically. Thanks to Stephan Broennimann for this nifty
  implementation.

  @code
  struct Bar {
      int i;
      int k;
      const char* data;

      struct Key;
      bool operator==(const Bar::Key& rhs) const;
  };

  struct Bar::Key {
      Key(int a, int b) : i(a), k(b) {}
      int i;
      int k;
  };

  bool Bar::operator==(const Bar::Key& key) const // definition
  {
      return i == key.i && k == key.k;
  }

  const Bar bars[] = {
      { 1, 1, "bar data 1" },
      { 1, 2, "bar data 2" },
      { 1, 3, "bar data 3" }
  };

  int main ( void ) {
      const Bar* bar = find(bars, Bar::Key(1, 3));
      if (bar) std::cout << bar->data << "\n";
      else std::cout << "Key not found.\n";
      return 0;
  }
  @endcode
*/
template <typename T, typename K, int N>
const T* find(T (&src)[N], const K& key) {
  auto rc = std::find(src, src + N, key);
  return rc == src + N ? nullptr : rc;
}

//! Utility function to convert the argument of any type to a string
template <typename T>
std::string toStringHelper(const T& arg, std::true_type) {
  return std::to_string(arg);
}

template <typename T>
std::string toStringHelper(const T& arg, std::false_type) {
  std::ostringstream os;
  os << arg;
  return os.str();
}
template <typename T>
std::string toString(const T& arg) {
  return toStringHelper(arg, std::is_integral<T>());
}

/*!
  @brief Utility function to convert a string to a value of type \c T.

  The string representation of the value must match that recognized by
  the input operator for \c T for this function to succeed.

  @param  s  String to convert
  @param  ok Output variable indicating the success of the operation.
  @return Returns the converted value and sets \em ok to \c true if the
          conversion was successful or \c false if not.
 */
template <typename T>
T stringTo(const std::string& s, bool& ok) {
  std::istringstream is(s);
  T tmp = T();
  ok = static_cast<bool>(is >> tmp);
  std::string rest;
  is >> std::skipws >> rest;
  if (!rest.empty())
    ok = false;
  return tmp;
}

/*!
  @brief Specialization of stringTo(const std::string& s, bool& ok) for \em bool.

  Handles the same string values as the XMP SDK. Converts the string to lowercase
  and returns \c true if it is "true", "t" or "1", and \c false if it is
  "false", "f" or "0".
 */
template <>
bool stringTo<bool>(const std::string& s, bool& ok);

}  // namespace Exiv2

#endif  // #ifndef TYPES_HPP_
