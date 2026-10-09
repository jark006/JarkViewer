// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef VALUE_HPP_
#define VALUE_HPP_

// *****************************************************************************
#include "exiv2lib_export.h"

// included header files
#include "types.hpp"

// + standard includes
#include <cmath>
#include <cstring>
#include <iomanip>
#include <map>
#include <memory>

// *****************************************************************************
// namespace extensions
namespace Exiv2 {
// *****************************************************************************
// class definitions

/*!
  @brief Common interface for all types of values used with metadata.

  The interface provides a uniform way to access values independent of
  their actual C++ type for simple tasks like reading the values from a
  string or data buffer.  For other tasks, like modifying values you may
  need to downcast it to a specific subclass to access its interface.
 */
class EXIV2API Value {
 public:
  //! Shortcut for a %Value auto pointer.
  using UniquePtr = std::unique_ptr<Value>;

  //! @name Creators
  //@{
  //! Constructor, taking a type id to initialize the base class with
  explicit Value(TypeId typeId);
  //! Virtual destructor.
  virtual ~Value() = default;
  //@}

  //! @name Manipulators
  //@{

  /// @brief Read the value from a character buffer.
  /// @param buf Pointer to the data buffer to read from
  /// @param len Number of bytes in the data buffer
  /// @param byteOrder Applicable byte order (little or big endian).
  /// @return 0 if successful.
  virtual int read(const byte* buf, size_t len, ByteOrder byteOrder) = 0;

  /*!
    @brief Set the value from a string buffer. The format of the string
           corresponds to that of the write() method, i.e., a string
           obtained through the write() method can be read by this
           function.

    @param buf The string to read from.

    @return 0 if successful.
   */
  virtual int read(const std::string& buf) = 0;
  /*!
    @brief Set the data area, if the value has one by copying (cloning)
           the buffer pointed to by buf.

    Values may have a data area, which can contain additional
    information besides the actual value. This method is used to set such
    a data area.

    @param buf Pointer to the source data area
    @param len Size of the data area
    @return Return -1 if the value has no data area, else 0.
   */
  virtual int setDataArea(const byte* buf, size_t len);
  //@}

  //! @name Accessors
  //@{
  //! Return the type identifier (Exif data format type).
  TypeId typeId() const {
    return type_;
  }
  /*!
    @brief Return an auto-pointer to a copy of itself (deep copy).
           The caller owns this copy and the auto-pointer ensures that
           it will be deleted.
   */
  UniquePtr clone() const {
    return UniquePtr(clone_());
  }
  /*!
    @brief Write value to a data buffer.

    The user must ensure that the buffer has enough memory. Otherwise
    the call results in undefined behaviour.

    @param buf Data buffer to write to.
    @param byteOrder Applicable byte order (little or big endian).
    @return Number of bytes written.
  */
  virtual size_t copy(byte* buf, ByteOrder byteOrder) const = 0;
  //! Return the number of components of the value
  virtual size_t count() const = 0;
  //! Return the size of the value in bytes
  virtual size_t size() const = 0;
  /*!
    @brief Write the value to an output stream. You do not usually have
           to use this function; it is used for the implementation of
           the output operator for %Value,
           operator<<(std::ostream &os, const Value &value).
  */
  virtual std::ostream& write(std::ostream& os) const = 0;
  /*!
    @brief Return the value as a string. Implemented in terms of
           write(std::ostream& os) const of the concrete class.
   */
  std::string toString() const;
  /*!
    @brief Return the <EM>n</EM>-th component of the value as a string.
           The default implementation returns toString(). The behaviour
           of this method may be undefined if there is no <EM>n</EM>-th
           component.
   */
  virtual std::string toString(size_t n) const;
  /*!
    @brief Convert the <EM>n</EM>-th component of the value to an int64_t.
           The behaviour of this method may be undefined if there is no
           <EM>n</EM>-th component.

    @return The converted value.
   */
  virtual int64_t toInt64(size_t n = 0) const = 0;
  /*!
    @brief Convert the <EM>n</EM>-th component of the value to a float.
           The behaviour of this method may be undefined if there is no
           <EM>n</EM>-th component.

    @return The converted value.
   */
  virtual uint32_t toUint32(size_t n = 0) const = 0;
  /*!
    @brief Convert the <EM>n</EM>-th component of the value to a float.
           The behaviour of this method may be undefined if there is no
           <EM>n</EM>-th component.

    @return The converted value.
   */
  virtual float toFloat(size_t n = 0) const = 0;
  /*!
    @brief Convert the <EM>n</EM>-th component of the value to a Rational.
           The behaviour of this method may be undefined if there is no
           <EM>n</EM>-th component.

    @return The converted value.
   */
  virtual Rational toRational(size_t n = 0) const = 0;
  //! Return the size of the data area, 0 if there is none.
  virtual size_t sizeDataArea() const;
  /*!
    @brief Return a copy of the data area if the value has one. The
           caller owns this copy and DataBuf ensures that it will be
           deleted.

    Values may have a data area, which can contain additional
    information besides the actual value. This method is used to access
    such a data area.

    @return A DataBuf containing a copy of the data area or an empty
            DataBuf if the value does not have a data area assigned.
   */
  virtual DataBuf dataArea() const;
  /*!
    @brief Check the \em ok status indicator. After a to<Type> conversion,
           this indicator shows whether the conversion was successful.
   */
  bool ok() const {
    return ok_;
  }
  //@}

  /*!
    @brief A (simple) factory to create a Value type.

    The following Value subclasses are created depending on typeId:<BR><BR>
    <TABLE>
    <TR><TD><B>typeId</B></TD><TD><B>%Value subclass</B></TD></TR>
    <TR><TD><CODE>invalidTypeId</CODE></TD><TD>%DataValue(invalidTypeId)</TD></TR>
    <TR><TD><CODE>unsignedByte</CODE></TD><TD>%DataValue(unsignedByte)</TD></TR>
    <TR><TD><CODE>asciiString</CODE></TD><TD>%AsciiValue</TD></TR>
    <TR><TD><CODE>string</CODE></TD><TD>%StringValue</TD></TR>
    <TR><TD><CODE>unsignedShort</CODE></TD><TD>%ValueType &lt; uint16_t &gt;</TD></TR>
    <TR><TD><CODE>unsignedLong</CODE></TD><TD>%ValueType &lt; uint32_t &gt;</TD></TR>
    <TR><TD><CODE>unsignedRational</CODE></TD><TD>%ValueType &lt; URational &gt;</TD></TR>
    <TR><TD><CODE>invalid6</CODE></TD><TD>%DataValue(invalid6)</TD></TR>
    <TR><TD><CODE>undefined</CODE></TD><TD>%DataValue</TD></TR>
    <TR><TD><CODE>signedShort</CODE></TD><TD>%ValueType &lt; int16_t &gt;</TD></TR>
    <TR><TD><CODE>signedLong</CODE></TD><TD>%ValueType &lt; int32_t &gt;</TD></TR>
    <TR><TD><CODE>signedRational</CODE></TD><TD>%ValueType &lt; Rational &gt;</TD></TR>
    <TR><TD><CODE>tiffFloat</CODE></TD><TD>%ValueType &lt; float &gt;</TD></TR>
    <TR><TD><CODE>tiffDouble</CODE></TD><TD>%ValueType &lt; double &gt;</TD></TR>
    <TR><TD><CODE>tiffIfd</CODE></TD><TD>%ValueType &lt; uint32_t &gt;</TD></TR>
    <TR><TD><CODE>date</CODE></TD><TD>%DateValue</TD></TR>
    <TR><TD><CODE>time</CODE></TD><TD>%TimeValue</TD></TR>
    <TR><TD><CODE>comment</CODE></TD><TD>%CommentValue</TD></TR>
    <TR><TD><CODE>xmpText</CODE></TD><TD>%XmpTextValue</TD></TR>
    <TR><TD><CODE>xmpBag</CODE></TD><TD>%XmpArrayValue</TD></TR>
    <TR><TD><CODE>xmpSeq</CODE></TD><TD>%XmpArrayValue</TD></TR>
    <TR><TD><CODE>xmpAlt</CODE></TD><TD>%XmpArrayValue</TD></TR>
    <TR><TD><CODE>langAlt</CODE></TD><TD>%LangAltValue</TD></TR>
    <TR><TD><EM>default</EM></TD><TD>%DataValue(typeId)</TD></TR>
    </TABLE>

    @param typeId Type of the value.
    @return Auto-pointer to the newly created Value. The caller owns this
            copy and the auto-pointer ensures that it will be deleted.
   */
  static UniquePtr create(TypeId typeId);

 protected:
  Value(const Value&) = default;
  /*!
    @brief Assignment operator. Protected so that it can only be used
           by subclasses but not directly.
   */
  Value& operator=(const Value&) = default;
  // DATA
  mutable bool ok_{true};  //!< Indicates the status of the previous to<Type> conversion

 private:
  //! Internal virtual copy constructor.
  virtual Value* clone_() const = 0;
  // DATA
  TypeId type_;  //!< Type of the data
};

//! Output operator for Value types
inline std::ostream& operator<<(std::ostream& os, const Value& value) {
  return value.write(os);
}

//! %Value for an undefined data type.
class EXIV2API DataValue : public Value {
 public:
  //! Shortcut for a %DataValue auto pointer.
  using UniquePtr = std::unique_ptr<DataValue>;

  explicit DataValue(TypeId typeId = undefined);

  DataValue(const byte* buf, size_t len, ByteOrder byteOrder = invalidByteOrder, TypeId typeId = undefined);

  //! @name Manipulators
  //@{
  int read(const byte* buf, size_t len, ByteOrder byteOrder = invalidByteOrder) override;
  //! Set the data from a string of integer values (e.g., "0 1 2 3")
  int read(const std::string& buf) override;
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const {
    return UniquePtr(clone_());
  }
  /*!
    @brief Write value to a character data buffer.

    @note The byte order is required by the interface but not used by this
          method, so just use the default.

    The user must ensure that the buffer has enough memory. Otherwise
    the call results in undefined behaviour.

    @param buf Data buffer to write to.
    @param byteOrder Byte order. Not needed.
    @return Number of characters written.
  */
  size_t copy(byte* buf, ByteOrder byteOrder = invalidByteOrder) const override;
  size_t count() const override;
  size_t size() const override;
  std::ostream& write(std::ostream& os) const override;
  /*!
    @brief Return the <EM>n</EM>-th component of the value as a string.
           The behaviour of this method may be undefined if there is no
           <EM>n</EM>-th component.
   */
  std::string toString(size_t n) const override;
  int64_t toInt64(size_t n = 0) const override;
  uint32_t toUint32(size_t n = 0) const override;
  float toFloat(size_t n = 0) const override;
  Rational toRational(size_t n = 0) const override;
  //@}

 private:
  //! Internal virtual copy constructor.
  DataValue* clone_() const override;

 public:
  //! Type used to store the data.
  using ValueType = std::vector<byte>;
  // DATA
  ValueType value_;  //!< Stores the data value

};  // class DataValue

/*!
  @brief Abstract base class for a string based %Value type.

  Uses a std::string to store the value and implements defaults for
  most operations.
 */
class EXIV2API StringValueBase : public Value {
  using Value::Value;

 public:
  //! Shortcut for a %StringValueBase auto pointer.
  using UniquePtr = std::unique_ptr<StringValueBase>;

  //! @name Creators
  //@{
  //! Constructor for subclasses
  StringValueBase(TypeId typeId, const std::string& buf);
  //@}

  //! @name Manipulators
  //@{
  //! Read the value from buf. This default implementation uses buf as it is.
  int read(const std::string& buf) override;
  int read(const byte* buf, size_t len, ByteOrder byteOrder = invalidByteOrder) override;
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const {
    return UniquePtr(clone_());
  }
  /*!
    @brief Write value to a character data buffer.

    The user must ensure that the buffer has enough memory. Otherwise
    the call results in undefined behaviour.

    @note The byte order is required by the interface but not used by this
          method, so just use the default.

    @param buf Data buffer to write to.
    @param byteOrder Byte order. Not used.
    @return Number of characters written.
  */
  size_t copy(byte* buf, ByteOrder byteOrder = invalidByteOrder) const override;
  size_t count() const override;
  size_t size() const override;
  int64_t toInt64(size_t n = 0) const override;
  uint32_t toUint32(size_t n = 0) const override;
  float toFloat(size_t n = 0) const override;
  Rational toRational(size_t n = 0) const override;
  std::ostream& write(std::ostream& os) const override;
  //@}

 protected:
  //! Internal virtual copy constructor.
  StringValueBase* clone_() const override = 0;

 public:
  // DATA
  std::string value_;  //!< Stores the string value.

};  // class StringValueBase

/*!
  @brief %Value for string type.

  This can be a plain Ascii string or a multiple byte encoded string. It is
  left to caller to decode and encode the string to and from readable
  text if that is required.
*/
class EXIV2API StringValue : public StringValueBase {
 public:
  //! Shortcut for a %StringValue auto pointer.
  using UniquePtr = std::unique_ptr<StringValue>;

  //! @name Creators
  //@{
  //! Default constructor.
  StringValue();
  //! Constructor
  explicit StringValue(const std::string& buf);
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const {
    return UniquePtr(clone_());
  }
  //@}

 private:
  //! Internal virtual copy constructor.
  StringValue* clone_() const override;

};  // class StringValue

/*!
  @brief %Value for an Ascii string type.

  This class is for null terminated single byte Ascii strings.
  This class also ensures that the string is null terminated.
 */
class EXIV2API AsciiValue : public StringValueBase {
 public:
  //! Shortcut for a %AsciiValue auto pointer.
  using UniquePtr = std::unique_ptr<AsciiValue>;

  //! @name Creators
  //@{
  //! Default constructor.
  AsciiValue();
  //! Constructor
  explicit AsciiValue(const std::string& buf);
  //@}

  //! @name Manipulators
  //@{
  using StringValueBase::read;
  /*!
    @brief Set the value to that of the string buf. Overrides base class
           to append a terminating '\\0' character if buf doesn't end
           with '\\0'.
   */
  int read(const std::string& buf) override;
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const {
    return UniquePtr(clone_());
  }
  /*!
    @brief Write the ASCII value up to the first '\\0' character to an
           output stream.  Any further characters are ignored and not
           written to the output stream.
  */
  std::ostream& write(std::ostream& os) const override;
  //@}

 private:
  //! Internal virtual copy constructor.
  AsciiValue* clone_() const override;

};  // class AsciiValue

/*!
  @brief %Value for an Exif comment.

  This can be a plain Ascii string or a multiple byte encoded string. The
  comment is expected to be encoded in the character set indicated (default
  undefined), but this is not checked. It is left to caller to decode and
  encode the string to and from readable text if that is required.
*/
class EXIV2API CommentValue : public StringValueBase {
 public:
  //! Character set identifiers for the character sets defined by %Exif
  enum CharsetId { ascii, jis, unicode, undefined, invalidCharsetId, lastCharsetId };
  //! Information pertaining to the defined character sets
  struct CharsetTable {
    CharsetId charsetId_;  //!< Charset id
    const char* name_;     //!< Name of the charset
    const char* code_;     //!< Code of the charset
  };

  //! Charset information lookup functions. Implemented as a static class.
  class EXIV2API CharsetInfo {
   public:
    //! Return the name for a charset id
    static const char* name(CharsetId charsetId);
    //! Return the code for a charset id
    static const char* code(CharsetId charsetId);
    //! Return the charset id for a name
    static CharsetId charsetIdByName(const std::string& name);
    //! Return the charset id for a code
    static CharsetId charsetIdByCode(const std::string& code);

   private:
    static const CharsetTable charsetTable_[];
  };  // class CharsetInfo

  //! Shortcut for a %CommentValue auto pointer.
  using UniquePtr = std::unique_ptr<CommentValue>;

  //! @name Creators
  //@{
  //! Default constructor.
  CommentValue();
  //! Constructor, uses read(const std::string& comment)
  explicit CommentValue(const std::string& comment);
  //@}

  //! @name Manipulators
  //@{
  /*!
    @brief Read the value from a comment

    The format of \em comment is:
    <BR>
    <CODE>[charset=["]Ascii|Jis|Unicode|Undefined["] ]comment</CODE>
    <BR>
    The default charset is Undefined.

    @return 0 if successful<BR>
            1 if an invalid character set is encountered
  */
  int read(const std::string& comment) override;
  int read(const byte* buf, size_t len, ByteOrder byteOrder) override;
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const {
    return UniquePtr(clone_());
  }
  size_t copy(byte* buf, ByteOrder byteOrder) const override;
  /*!
    @brief Write the comment in a format which can be read by
    read(const std::string& comment).
   */
  std::ostream& write(std::ostream& os) const override;
  /*!
    @brief Return the comment (without a charset="..." prefix)

    The comment is decoded to UTF-8. For Exif UNICODE comments, the
    function makes an attempt to correctly determine the character
    encoding of the value. Alternatively, the optional \em encoding
    parameter can be used to specify it.

    @param encoding Optional argument to specify the character encoding
        that the comment is encoded in, as an iconv(3) name. Only used
        for Exif UNICODE comments.

    @return A string containing the comment converted to UTF-8.
   */
  std::string comment(const char* encoding = nullptr) const;
  /*!
    @brief Determine the character encoding that was used to encode the
        UNICODE comment value as an iconv(3) name.

    If the comment \em c starts with a BOM, the BOM is interpreted and
    removed from the string.

    Todo: Implement rules to guess if the comment is UTF-8 encoded.
   */
  const char* detectCharset(std::string& c) const;
  //! Return the Exif charset id of the comment
  CharsetId charsetId() const;
  //@}

 private:
  //! Internal virtual copy constructor.
  CommentValue* clone_() const override;

 public:
  // DATA
  ByteOrder byteOrder_{littleEndian};  //!< Byte order of the comment string that was read

};  // class CommentValue

/*!
  @brief Base class for all Exiv2 values used to store XMP property values.
 */
class EXIV2API XmpValue : public Value {
  using Value::Value;

 public:
  //! Shortcut for a %XmpValue auto pointer.
  using UniquePtr = std::unique_ptr<XmpValue>;

  //! XMP array types.
  enum XmpArrayType { xaNone, xaAlt, xaBag, xaSeq };
  //! XMP structure indicator.
  enum XmpStruct { xsNone, xsStruct };

  //! @name Accessors
  //@{
  //! Return XMP array type, indicates if an XMP value is an array.
  XmpArrayType xmpArrayType() const;
  //! Return XMP struct, indicates if an XMP value is a structure.
  XmpStruct xmpStruct() const;
  size_t size() const override;
  /*!
    @brief Write value to a character data buffer.

    The user must ensure that the buffer has enough memory. Otherwise
    the call results in undefined behaviour.

    @note The byte order is required by the interface but not used by this
          method, so just use the default.

    @param buf Data buffer to write to.
    @param byteOrder Byte order. Not used.
    @return Number of characters written.
  */
  size_t copy(byte* buf, ByteOrder byteOrder = invalidByteOrder) const override;
  //@}

  //! @name Manipulators
  //@{
  //! Set the XMP array type to indicate that an XMP value is an array.
  void setXmpArrayType(XmpArrayType xmpArrayType);
  //! Set the XMP struct type to indicate that an XMP value is a structure.
  void setXmpStruct(XmpStruct xmpStruct = xsStruct);

  /// @note Uses read(const std::string& buf)
  int read(const byte* buf, size_t len, ByteOrder byteOrder = invalidByteOrder) override;
  int read(const std::string& buf) override = 0;
  //@}

  /*!
    @brief Return XMP array type for an array Value TypeId, xaNone if
           \em typeId is not an XMP array value type.
   */
  static XmpArrayType xmpArrayType(TypeId typeId);

 private:
  // DATA
  XmpArrayType xmpArrayType_{xaNone};  //!< Type of XMP array
  XmpStruct xmpStruct_{xsNone};        //!< XMP structure indicator

};  // class XmpValue

/*!
  @brief %Value type suitable for simple XMP properties and
         XMP nodes of complex types which are not parsed into
         specific values.

  Uses a std::string to store the value.
 */
class EXIV2API XmpTextValue : public XmpValue {
 public:
  //! Shortcut for a %XmpTextValue auto pointer.
  using UniquePtr = std::unique_ptr<XmpTextValue>;

  //! @name Creators
  //@{
  //! Constructor.
  XmpTextValue();
  //! Constructor, reads the value from a string.
  explicit XmpTextValue(const std::string& buf);
  //@}

  //! @name Manipulators
  //@{
  using XmpValue::read;
  /*!
    @brief Read a simple property value from \em buf to set the value.

    Sets the value to the contents of \em buf. A optional keyword,
    \em type is supported to set the XMP value type. This is useful for
    complex value types for which Exiv2 does not have direct support.

    The format of \em buf is:
    <BR>
    <CODE>[type=["]Alt|Bag|Seq|Struct["] ]text</CODE>
    <BR>

    @return 0 if successful.
   */

  int read(const std::string& buf) override;
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const;
  size_t size() const override;
  size_t count() const override;
  /*!
    @brief Convert the value to an int64_t.
           The optional parameter \em n is not used and is ignored.

    @return The converted value.
   */
  int64_t toInt64(size_t n = 0) const override;
  /*!
    @brief Convert the value to an uint32_t.
           The optional parameter \em n is not used and is ignored.

    @return The converted value.
   */
  uint32_t toUint32(size_t n = 0) const override;
  /*!
    @brief Convert the value to a float.
           The optional parameter \em n is not used and is ignored.

    @return The converted value.
   */
  float toFloat(size_t n = 0) const override;
  /*!
    @brief Convert the value to a Rational.
           The optional parameter \em n is not used and is ignored.

    @return The converted value.
   */
  Rational toRational(size_t n = 0) const override;
  std::ostream& write(std::ostream& os) const override;
  //@}

 private:
  //! Internal virtual copy constructor.
  XmpTextValue* clone_() const override;

 public:
  // DATA
  std::string value_;  //!< Stores the string values.

};  // class XmpTextValue

/*!
  @brief %Value type for simple arrays. Each item in the array is a simple
         value, without qualifiers. The array may be an ordered (\em seq),
         unordered (\em bag) or alternative array (\em alt). The array
         items must not contain qualifiers. For language alternatives use
         LangAltValue.

  Uses a vector of std::string to store the value(s).
 */
class EXIV2API XmpArrayValue : public XmpValue {
 public:
  //! Shortcut for a %XmpArrayValue auto pointer.
  using UniquePtr = std::unique_ptr<XmpArrayValue>;

  //! @name Creators
  //@{
  //! Constructor. \em typeId can be one of xmpBag, xmpSeq or xmpAlt.
  explicit XmpArrayValue(TypeId typeId = xmpBag);
  //@}

  //! @name Manipulators
  //@{
  using XmpValue::read;
  /*!
    @brief Read a simple property value from \em buf and append it
           to the value.

    Appends \em buf to the value after the last existing array element.
    Subsequent calls will therefore populate multiple array elements in
    the order they are read.

    @return 0 if successful.
   */
  int read(const std::string& buf) override;
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const;
  size_t count() const override;
  /*!
    @brief Return the <EM>n</EM>-th component of the value as a string.
           The behaviour of this method may be undefined if there is no
           <EM>n</EM>-th component.
   */
  std::string toString(size_t n) const override;
  int64_t toInt64(size_t n = 0) const override;
  uint32_t toUint32(size_t n = 0) const override;
  float toFloat(size_t n = 0) const override;
  Rational toRational(size_t n = 0) const override;
  /*!
    @brief Write all elements of the value to \em os, separated by commas.

    @note The output of this method cannot directly be used as the parameter
          for read().
   */
  std::ostream& write(std::ostream& os) const override;
  //@}

 private:
  //! Internal virtual copy constructor.
  XmpArrayValue* clone_() const override;

  std::vector<std::string> value_;  //!< Stores the string values.

};  // class XmpArrayValue

/*!
  @brief %LangAltValueComparator

  #1058
  https://www.adobe.com/content/dam/Adobe/en/devnet/xmp/pdfs/XMPSpecificationPart1.pdf
  XMP spec chapter B.4 (page 42) the xml:lang qualifier is to be compared case insensitive.
  */
struct LangAltValueComparator {
  //! LangAltValueComparator comparison case insensitive function
  bool operator()(const std::string& str1, const std::string& str2) const {
    int result = str1.size() < str2.size() ? 1 : str1.size() > str2.size() ? -1 : 0;
    if (result == 0) {
      for (auto c1 = str1.begin(), c2 = str2.begin(); result == 0 && c1 != str1.end(); ++c1, ++c2) {
        result = tolower(*c1) < tolower(*c2) ? 1 : tolower(*c1) > tolower(*c2) ? -1 : 0;
      }
    }
    return result < 0;
  }
};

/*!
  @brief %Value type for XMP language alternative properties.

  A language alternative is an array consisting of simple text values,
  each of which has a language qualifier.
 */
class EXIV2API LangAltValue : public XmpValue {
 public:
  //! Shortcut for a %LangAltValue auto pointer.
  using UniquePtr = std::unique_ptr<LangAltValue>;

  //! @name Creators
  //@{
  //! Constructor.
  LangAltValue();
  //! Constructor, reads the value from a string.
  explicit LangAltValue(const std::string& buf);
  //@}

  //! @name Manipulators
  //@{
  using XmpValue::read;
  /*!
    @brief Read a simple property value from \em buf and append it
           to the value.

    Appends \em buf to the value after the last existing array element.
    Subsequent calls will therefore populate multiple array elements in
    the order they are read.

    The format of \em buf is:
    <BR>
    <CODE>[lang=["]language code["] ]text</CODE>
    <BR>
    The XMP default language code <CODE>x-default</CODE> is used if
    \em buf doesn't start with the keyword <CODE>lang</CODE>.

    @return 0 if successful.
   */
  int read(const std::string& buf) override;
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const;
  size_t count() const override;
  /*!
    @brief Return the text value associated with the default language
           qualifier \c x-default. The parameter \em n is not used, but
           it is suggested that only 0 is passed in. Returns an empty
           string and sets the ok-flag to \c false if there is no
           default value.
   */
  std::string toString(size_t n) const override;
  /*!
    @brief Return the text value associated with the language qualifier
           \em qualifier. Returns an empty string and sets the ok-flag
           to \c false if there is no entry for the language qualifier.
   */
  std::string toString(const std::string& qualifier) const;
  int64_t toInt64(size_t n = 0) const override;
  uint32_t toUint32(size_t n = 0) const override;
  float toFloat(size_t n = 0) const override;
  Rational toRational(size_t n = 0) const override;
  /*!
    @brief Write all elements of the value to \em os, separated by commas.

    @note The output of this method cannot directly be used as the parameter
          for read().
   */
  std::ostream& write(std::ostream& os) const override;
  //@}

 private:
  //! Internal virtual copy constructor.
  LangAltValue* clone_() const override;

 public:
  //! Type used to store language alternative arrays.
  using ValueType = std::map<std::string, std::string, LangAltValueComparator>;
  // DATA
  /*!
    @brief Map to store the language alternative values. The language
           qualifier is used as the key for the map entries.
   */
  ValueType value_;

};  // class LangAltValue

/*!
  @brief %Value for simple ISO 8601 dates

  This class is limited to parsing simple date strings in the ISO 8601
  format CCYYMMDD (century, year, month, day).
 */
class EXIV2API DateValue : public Value {
 public:
  //! Shortcut for a %DateValue auto pointer.
  using UniquePtr = std::unique_ptr<DateValue>;

  //! @name Creators
  //@{
  //! Default constructor.
  DateValue();
  //! Constructor
  DateValue(int32_t year, int32_t month, int32_t day);
  //@}

  //! Simple Date helper structure
  struct EXIV2API Date {
    int32_t year;   //!< Year
    int32_t month;  //!< Month
    int32_t day;    //!< Day
  };

  //! @name Manipulators
  //@{

  /// @return 0 if successful<BR>
  ///         1 in case of an unsupported date format
  int read(const byte* buf, size_t len, ByteOrder byteOrder = invalidByteOrder) override;
  /*!
    @brief Set the value to that of the string buf.

    @param buf String containing the date

    @return 0 if successful<BR>
            1 in case of an unsupported date format
   */
  int read(const std::string& buf) override;
  //! Set the date
  void setDate(const Date& src);
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const {
    return UniquePtr(clone_());
  }
  /*!
    @brief Write value to a character data buffer.

    The user must ensure that the buffer has enough memory. Otherwise
    the call results in undefined behaviour.

    @note The byte order is required by the interface but not used by this
          method, so just use the default.

    @param buf Data buffer to write to.
    @param byteOrder Byte order. Not used.
    @return Number of characters written.
  */
  size_t copy(byte* buf, ByteOrder byteOrder = invalidByteOrder) const override;

  //! Return date struct containing date information
  virtual const Date& getDate() const;
  size_t count() const override;
  size_t size() const override;
  std::ostream& write(std::ostream& os) const override;
  //! Return the value as a UNIX calendar time converted to int64_t.
  int64_t toInt64(size_t n = 0) const override;
  //! Return the value as a UNIX calendar time converted to uint32_t.
  uint32_t toUint32(size_t n = 0) const override;
  //! Return the value as a UNIX calendar time converted to float.
  float toFloat(size_t n = 0) const override;
  //! Return the value as a UNIX calendar time converted to Rational.
  Rational toRational(size_t n = 0) const override;
  //@}

 private:
  //! Internal virtual copy constructor.
  DateValue* clone_() const override;

  // DATA
  Date date_;

};  // class DateValue

/*!
 @brief %Value for simple ISO 8601 times.

 This class is limited to handling simple time strings in the ISO 8601
 format HHMMSS±HHMM where HHMMSS refers to local hour, minute and
 seconds and ±HHMM refers to hours and minutes ahead or behind
 Universal Coordinated Time.
 */
class EXIV2API TimeValue : public Value {
 public:
  //! Shortcut for a %TimeValue auto pointer.
  using UniquePtr = std::unique_ptr<TimeValue>;

  //! @name Creators
  //@{
  //! Default constructor.
  TimeValue();
  //! Constructor
  TimeValue(int32_t hour, int32_t minute, int32_t second = 0, int32_t tzHour = 0, int32_t tzMinute = 0);
  //@}

  //! Simple Time helper structure
  struct Time {
    int32_t hour;      //!< Hour
    int32_t minute;    //!< Minute
    int32_t second;    //!< Second
    int32_t tzHour;    //!< Hours ahead or behind UTC
    int32_t tzMinute;  //!< Minutes ahead or behind UTC
  };

  //! @name Manipulators
  //@{

  ///  @return 0 if successful<BR>
  ///          1 in case of an unsupported time format
  int read(const byte* buf, size_t len, ByteOrder byteOrder = invalidByteOrder) override;
  /*!
    @brief Set the value to that of the string buf.

    @param buf String containing the time.

    @return 0 if successful<BR>
            1 in case of an unsupported time format
   */
  int read(const std::string& buf) override;
  //! Set the time
  void setTime(const Time& src);
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const {
    return UniquePtr(clone_());
  }
  /*!
    @brief Write value to a character data buffer.

    The user must ensure that the buffer has enough memory. Otherwise
    the call results in undefined behaviour.

    @note The byte order is required by the interface but not used by this
          method, so just use the default.

    @param buf Data buffer to write to.
    @param byteOrder Byte order. Not used.
    @return Number of characters written.
  */
  size_t copy(byte* buf, ByteOrder byteOrder = invalidByteOrder) const override;
  //! Return time struct containing time information
  virtual const Time& getTime() const;
  size_t count() const override;
  size_t size() const override;
  std::ostream& write(std::ostream& os) const override;
  //! Returns number of seconds in the day in UTC.
  int64_t toInt64(size_t n = 0) const override;
  //! Returns number of seconds in the day in UTC.
  uint32_t toUint32(size_t n = 0) const override;
  //! Returns number of seconds in the day in UTC converted to float.
  float toFloat(size_t n = 0) const override;
  //! Returns number of seconds in the day in UTC converted to Rational.
  Rational toRational(size_t n = 0) const override;
  //@}

 private:
  //! @name Accessors
  //@{
  //! Internal virtual copy constructor.
  TimeValue* clone_() const override;
  //@}

  // DATA
  Time time_;

};  // class TimeValue

//! Template to determine the TypeId for a type T
template <typename T>
TypeId getType();

//! Specialization for an unsigned short
template <>
inline TypeId getType<uint16_t>() {
  return unsignedShort;
}
//! Specialization for an unsigned long
template <>
inline TypeId getType<uint32_t>() {
  return unsignedLong;
}
//! Specialization for an unsigned rational
template <>
inline TypeId getType<URational>() {
  return unsignedRational;
}
//! Specialization for a signed short
template <>
inline TypeId getType<int16_t>() {
  return signedShort;
}
//! Specialization for a signed long
template <>
inline TypeId getType<int32_t>() {
  return signedLong;
}
//! Specialization for a signed rational
template <>
inline TypeId getType<Rational>() {
  return signedRational;
}
//! Specialization for a float
template <>
inline TypeId getType<float>() {
  return tiffFloat;
}
//! Specialization for a double
template <>
inline TypeId getType<double>() {
  return tiffDouble;
}

// No default implementation: let the compiler/linker complain
// template<typename T> inline TypeId getType() { return invalid; }

/*!
  @brief Template for a %Value of a basic type. This is used for unsigned
         and signed short, long and rationals.
 */
template <typename T>
class ValueType : public Value {
  using Value::Value;

 public:
  //! Shortcut for a %ValueType\<T\> auto pointer.
  using UniquePtr = std::unique_ptr<ValueType<T>>;

  //! @name Creators
  //@{
  //! Default Constructor.
  ValueType();
  //! Constructor.
  ValueType(const byte* buf, size_t len, ByteOrder byteOrder, TypeId typeId = getType<T>());
  //! Constructor.
  explicit ValueType(const T& val, TypeId typeId = getType<T>());
  //! Copy constructor
  ValueType(const ValueType<T>& rhs);
  //@}

  //! @name Manipulators
  //@{
  //! Assignment operator.
  ValueType<T>& operator=(const ValueType<T>& rhs);
  int read(const byte* buf, size_t len, ByteOrder byteOrder) override;
  /*!
    @brief Set the data from a string of values of type T (e.g.,
           "0 1 2 3" or "1/2 1/3 1/4" depending on what T is).
           Generally, the accepted input format is the same as that
           produced by the write() method.
   */
  int read(const std::string& buf) override;
  /*!
    @brief Set the data area. This method copies (clones) the buffer
           pointed to by buf.
   */
  int setDataArea(const byte* buf, size_t len) override;
  //@}

  //! @name Accessors
  //@{
  UniquePtr clone() const {
    return UniquePtr(clone_());
  }
  size_t copy(byte* buf, ByteOrder byteOrder) const override;
  size_t count() const override;
  size_t size() const override;
  std::ostream& write(std::ostream& os) const override;
  /*!
    @brief Return the <EM>n</EM>-th component of the value as a string.
           The behaviour of this method may be undefined if there is no
           <EM>n</EM>-th
           component.
   */
  std::string toString(size_t n) const override;
  int64_t toInt64(size_t n = 0) const override;
  uint32_t toUint32(size_t n = 0) const override;
  float toFloat(size_t n = 0) const override;
  Rational toRational(size_t n = 0) const override;
  //! Return the size of the data area.
  size_t sizeDataArea() const override;
  /*!
    @brief Return a copy of the data area in a DataBuf. The caller owns
           this copy and DataBuf ensures that it will be deleted.
   */
  DataBuf dataArea() const override;
  //@}

  //! Container for values
  using ValueList = std::vector<T>;
  //! Iterator type defined for convenience.
  using iterator = typename std::vector<T>::iterator;
  //! Const iterator type defined for convenience.
  using const_iterator = typename std::vector<T>::const_iterator;

  // DATA
  /*!
    @brief The container for all values. In your application, if you know
           what subclass of Value you're dealing with (and possibly the T)
           then you can access this STL container through the usual
           standard library functions.
   */
  ValueList value_;

 private:
  //! Utility for toInt64, toUint32, etc.
  template <typename I>
  inline I float_to_integer_helper(size_t n) const {
    const auto v = value_.at(n);
    if (static_cast<decltype(v)>(std::numeric_limits<I>::min()) <= v &&
        v <= static_cast<decltype(v)>(std::numeric_limits<I>::max())) {
      return static_cast<I>(std::lround(v));
    }
    return 0;
  }

  //! Utility for toInt64, toUint32, etc.
  template <typename I>
  inline I rational_to_integer_helper(size_t n) const {
    auto a = value_.at(n).first;
    auto b = value_.at(n).second;

    // Protect against divide-by-zero.
    if (b <= 0) {
      return 0;
    }

    // Check for integer overflow.
#ifdef __cpp_if_constexpr
    if constexpr (std::is_signed_v<I> == std::is_signed_v<decltype(a)>) {
#else
    if (std::is_signed<I>::value == std::is_signed<decltype(a)>::value) {
#endif
      // conversion does not change sign
      const auto imin = std::numeric_limits<I>::min();
      const auto imax = std::numeric_limits<I>::max();
      if (imax < b || a < imin || imax < a) {
        return 0;
      }
#ifdef __cpp_if_constexpr
    } else if constexpr (std::is_signed_v<I>) {
#else
    } else if (std::is_signed<I>::value) {
#endif
      // conversion is from unsigned to signed
#if __cplusplus >= 201402L || (defined(_MSVC_LANG) && (_MSVC_LANG >= 201402L))
      const auto imax = static_cast<std::make_unsigned_t<I>>(std::numeric_limits<I>::max());
#else
      const auto imax = static_cast<typename std::make_unsigned<I>::type>(std::numeric_limits<I>::max());
#endif
      if (imax < b || imax < a) {
        return 0;
      }
    } else {
      // conversion is from signed to unsigned
      const auto imax = std::numeric_limits<I>::max();
      if (a < 0) {
        return 0;
      }
      // Inputs are not negative so convert them to unsigned.
#if __cplusplus >= 201402L || (defined(_MSVC_LANG) && (_MSVC_LANG >= 201402L))
      const auto a_u = static_cast<std::make_unsigned_t<decltype(a)>>(a);
      const auto b_u = static_cast<std::make_unsigned_t<decltype(b)>>(b);
#else
      const auto a_u = static_cast<typename std::make_unsigned<decltype(a)>::type>(a);
      const auto b_u = static_cast<typename std::make_unsigned<decltype(b)>::type>(b);
#endif
      if (imax < b_u || imax < a_u) {
        return 0;
      }
    }

    return static_cast<I>(a) / static_cast<I>(b);
  }

  //! Internal virtual copy constructor.
  ValueType<T>* clone_() const override;

  // DATA
  //! Pointer to the buffer, nullptr if none has been allocated
  Blob pDataArea_;
};  // class ValueType

//! Unsigned short value type
using UShortValue = ValueType<uint16_t>;
//! Unsigned long value type
using ULongValue = ValueType<uint32_t>;
//! Unsigned rational value type
using URationalValue = ValueType<URational>;
//! Signed short value type
using ShortValue = ValueType<int16_t>;
//! Signed long value type
using LongValue = ValueType<int32_t>;
//! Signed rational value type
using RationalValue = ValueType<Rational>;
//! Float value type
using FloatValue = ValueType<float>;
//! Double value type
using DoubleValue = ValueType<double>;

// *****************************************************************************
// free functions, template and inline definitions

/*!
  @brief Read a value of type T from the data buffer.

  We need this template function for the ValueType template classes.
  There are only specializations of this function available; no default
  implementation is provided.

  @param buf Pointer to the data buffer to read from.
  @param byteOrder Applicable byte order (little or big endian).
  @return A value of type T.
 */
template <typename T>
T getValue(const byte* buf, ByteOrder byteOrder);
// Specialization for a 2 byte unsigned short value.
template <>
inline uint16_t getValue(const byte* buf, ByteOrder byteOrder) {
  return getUShort(buf, byteOrder);
}
// Specialization for a 4 byte unsigned long value.
template <>
inline uint32_t getValue(const byte* buf, ByteOrder byteOrder) {
  return getULong(buf, byteOrder);
}
// Specialization for an 8 byte unsigned rational value.
template <>
inline URational getValue(const byte* buf, ByteOrder byteOrder) {
  return getURational(buf, byteOrder);
}
// Specialization for a 2 byte signed short value.
template <>
inline int16_t getValue(const byte* buf, ByteOrder byteOrder) {
  return getShort(buf, byteOrder);
}
// Specialization for a 4 byte signed long value.
template <>
inline int32_t getValue(const byte* buf, ByteOrder byteOrder) {
  return getLong(buf, byteOrder);
}
// Specialization for an 8 byte signed rational value.
template <>
inline Rational getValue(const byte* buf, ByteOrder byteOrder) {
  return getRational(buf, byteOrder);
}
// Specialization for a 4 byte float value.
template <>
inline float getValue(const byte* buf, ByteOrder byteOrder) {
  return getFloat(buf, byteOrder);
}
// Specialization for a 8 byte double value.
template <>
inline double getValue(const byte* buf, ByteOrder byteOrder) {
  return getDouble(buf, byteOrder);
}

/*!
  @brief Convert a value of type T to data, write the data to the data buffer.

  We need this template function for the ValueType template classes.
  There are only specializations of this function available; no default
  implementation is provided.

  @param buf Pointer to the data buffer to write to.
  @param t Value to be converted.
  @param byteOrder Applicable byte order (little or big endian).
  @return The number of bytes written to the buffer.
 */
template <typename T>
size_t toData(byte* buf, T t, ByteOrder byteOrder);
/*!
  @brief Specialization to write an unsigned short to the data buffer.
         Return the number of bytes written.
 */
template <>
inline size_t toData(byte* buf, uint16_t t, ByteOrder byteOrder) {
  return us2Data(buf, t, byteOrder);
}
/*!
  @brief Specialization to write an unsigned long to the data buffer.
         Return the number of bytes written.
 */
template <>
inline size_t toData(byte* buf, uint32_t t, ByteOrder byteOrder) {
  return ul2Data(buf, t, byteOrder);
}
/*!
  @brief Specialization to write an unsigned rational to the data buffer.
         Return the number of bytes written.
 */
template <>
inline size_t toData(byte* buf, URational t, ByteOrder byteOrder) {
  return ur2Data(buf, t, byteOrder);
}
/*!
  @brief Specialization to write a signed short to the data buffer.
         Return the number of bytes written.
 */
template <>
inline size_t toData(byte* buf, int16_t t, ByteOrder byteOrder) {
  return s2Data(buf, t, byteOrder);
}
/*!
  @brief Specialization to write a signed long to the data buffer.
         Return the number of bytes written.
 */
template <>
inline size_t toData(byte* buf, int32_t t, ByteOrder byteOrder) {
  return l2Data(buf, t, byteOrder);
}
/*!
  @brief Specialization to write a signed rational to the data buffer.
         Return the number of bytes written.
 */
template <>
inline size_t toData(byte* buf, Rational t, ByteOrder byteOrder) {
  return r2Data(buf, t, byteOrder);
}
/*!
  @brief Specialization to write a float to the data buffer.
         Return the number of bytes written.
 */
template <>
inline size_t toData(byte* buf, float t, ByteOrder byteOrder) {
  return f2Data(buf, t, byteOrder);
}
/*!
  @brief Specialization to write a double to the data buffer.
         Return the number of bytes written.
 */
template <>
inline size_t toData(byte* buf, double t, ByteOrder byteOrder) {
  return d2Data(buf, t, byteOrder);
}

template <typename T>
ValueType<T>::ValueType() : Value(getType<T>()) {
}

template <typename T>
ValueType<T>::ValueType(const byte* buf, size_t len, ByteOrder byteOrder, TypeId typeId) : Value(typeId) {
  read(buf, len, byteOrder);
}

template <typename T>
ValueType<T>::ValueType(const T& val, TypeId typeId) : Value(typeId) {
  value_.push_back(val);
}

template <typename T>
ValueType<T>::ValueType(const ValueType<T>& rhs) : Value(rhs.typeId()), value_(rhs.value_) {
  if (!rhs.pDataArea_.empty())
    pDataArea_ = rhs.pDataArea_;
}

template <typename T>
ValueType<T>& ValueType<T>::operator=(const ValueType<T>& rhs) {
  if (this == &rhs)
    return *this;
  Value::operator=(rhs);
  value_ = rhs.value_;

  if (!rhs.pDataArea_.empty())
    pDataArea_ = rhs.pDataArea_;
  else
    pDataArea_.clear();

  return *this;
}

template <typename T>
int ValueType<T>::read(const byte* buf, size_t len, ByteOrder byteOrder) {
  value_.clear();
  size_t ts = TypeInfo::typeSize(typeId());
  if (ts > 0 && len % ts != 0)
    len = (len / ts) * ts;
  for (size_t i = 0; i < len; i += ts) {
    value_.push_back(getValue<T>(buf + i, byteOrder));
  }
  return 0;
}

template <typename T>
int ValueType<T>::read(const std::string& buf) {
  std::istringstream is(buf);
  T tmp = T();
  ValueList val;
  while (!(is.eof())) {
    is >> tmp;
    if (is.fail())
      return 1;
    val.push_back(tmp);
  }
  value_.swap(val);
  return 0;
}

template <typename T>
size_t ValueType<T>::copy(byte* buf, ByteOrder byteOrder) const {
  size_t offset = 0;
  for (auto i = value_.begin(); i != value_.end(); ++i) {
    offset += toData(buf + offset, *i, byteOrder);
  }
  return offset;
}

template <typename T>
size_t ValueType<T>::count() const {
  return value_.size();
}

template <typename T>
size_t ValueType<T>::size() const {
  return TypeInfo::typeSize(typeId()) * value_.size();
}

template <typename T>
ValueType<T>* ValueType<T>::clone_() const {
  return new ValueType<T>(*this);
}

template <typename T>
std::ostream& ValueType<T>::write(std::ostream& os) const {
  auto end = value_.end();
  auto i = value_.begin();
  while (i != end) {
    os << std::setprecision(15) << *i;
    if (++i != end)
      os << " ";
  }
  return os;
}

template <typename T>
std::string ValueType<T>::toString(size_t n) const {
  ok_ = true;
  return Exiv2::toString<T>(value_.at(n));
}

// Default implementation
template <typename T>
int64_t ValueType<T>::toInt64(size_t n) const {
  ok_ = true;
  return static_cast<int64_t>(value_.at(n));
}
template <typename T>
uint32_t ValueType<T>::toUint32(size_t n) const {
  ok_ = true;
  return static_cast<uint32_t>(value_.at(n));
}
// #55 crash when value_.at(n).first == LONG_MIN
#define LARGE_INT 1000000
// Specialization for double
template <>
inline int64_t ValueType<double>::toInt64(size_t n) const {
  return float_to_integer_helper<int64_t>(n);
}

template <>
inline uint32_t ValueType<double>::toUint32(size_t n) const {
  return float_to_integer_helper<uint32_t>(n);
}
// Specialization for float
template <>
inline int64_t ValueType<float>::toInt64(size_t n) const {
  return float_to_integer_helper<int64_t>(n);
}
template <>
inline uint32_t ValueType<float>::toUint32(size_t n) const {
  return float_to_integer_helper<uint32_t>(n);
}
// Specialization for rational
template <>
inline int64_t ValueType<Rational>::toInt64(size_t n) const {
  return rational_to_integer_helper<int64_t>(n);
}
template <>
inline uint32_t ValueType<Rational>::toUint32(size_t n) const {
  return rational_to_integer_helper<uint32_t>(n);
}
// Specialization for unsigned rational
template <>
inline int64_t ValueType<URational>::toInt64(size_t n) const {
  return rational_to_integer_helper<int64_t>(n);
}
template <>
inline uint32_t ValueType<URational>::toUint32(size_t n) const {
  return rational_to_integer_helper<uint32_t>(n);
}
// Default implementation
template <typename T>
float ValueType<T>::toFloat(size_t n) const {
  ok_ = true;
  return static_cast<float>(value_.at(n));
}
// Specialization for rational
template <>
inline float ValueType<Rational>::toFloat(size_t n) const {
  ok_ = (value_.at(n).second != 0);
  if (!ok_)
    return 0.0f;
  return static_cast<float>(value_.at(n).first) / value_.at(n).second;
}
// Specialization for unsigned rational
template <>
inline float ValueType<URational>::toFloat(size_t n) const {
  ok_ = (value_.at(n).second != 0);
  if (!ok_)
    return 0.0f;
  return static_cast<float>(value_.at(n).first) / value_.at(n).second;
}
// Default implementation
template <typename T>
Rational ValueType<T>::toRational(size_t n) const {
  ok_ = true;
  return {value_.at(n), 1};
}
// Specialization for rational
template <>
inline Rational ValueType<Rational>::toRational(size_t n) const {
  ok_ = true;
  return {value_.at(n).first, value_.at(n).second};
}
// Specialization for unsigned rational
template <>
inline Rational ValueType<URational>::toRational(size_t n) const {
  ok_ = true;
  return {value_.at(n).first, value_.at(n).second};
}
// Specialization for float.
template <>
inline Rational ValueType<float>::toRational(size_t n) const {
  ok_ = true;
  // Warning: This is a very simple conversion, see floatToRationalCast()
  return floatToRationalCast(value_.at(n));
}
// Specialization for double.
template <>
inline Rational ValueType<double>::toRational(size_t n) const {
  ok_ = true;
  // Warning: This is a very simple conversion, see floatToRationalCast()
  return floatToRationalCast(static_cast<float>(value_.at(n)));
}

template <typename T>
size_t ValueType<T>::sizeDataArea() const {
  return pDataArea_.size();
}

template <typename T>
DataBuf ValueType<T>::dataArea() const {
  return {pDataArea_.data(), pDataArea_.size()};
}

template <typename T>
int ValueType<T>::setDataArea(const byte* buf, size_t len) {
  if (len > 0)
    pDataArea_ = Blob(buf, buf + len);
  else
    pDataArea_.clear();
  return 0;
}
}  // namespace Exiv2

#endif  // #ifndef VALUE_HPP_
