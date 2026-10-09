// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef IMAGE_HPP_
#define IMAGE_HPP_

// *****************************************************************************
#include "exiv2lib_export.h"

// included header files
#include "basicio.hpp"
#include "exif.hpp"
#include "image_types.hpp"
#include "iptc.hpp"
#include "xmp_exiv2.hpp"

// *****************************************************************************
// namespace extensions
namespace Exiv2 {
// *****************************************************************************
// class definitions

//! Native preview information. This is meant to be used only by the PreviewManager.
struct NativePreview {
  size_t position_{};     //!< Position
  size_t size_{};         //!< Size
  size_t width_{};        //!< Width
  size_t height_{};       //!< Height
  std::string filter_;    //!< Filter
  std::string mimeType_;  //!< MIME type
};

//! List of native previews. This is meant to be used only by the PreviewManager.
using NativePreviewList = std::vector<NativePreview>;

/*!
  @brief Options for printStructure
 */
enum PrintStructureOption { kpsNone, kpsBasic, kpsXMP, kpsRecursive, kpsIccProfile, kpsIptcErase };

/*!
  @brief Abstract base class defining the interface for an image. This is
     the top-level interface to the Exiv2 library.

  Image has containers to store image metadata and subclasses implement
  read and save metadata from and to specific image formats.<BR>
  Most client apps will obtain an Image instance by calling a static
  ImageFactory method. The Image class can then be used to to read, write,
  and save metadata.
 */
class EXIV2API Image {
 public:
  //! Image auto_ptr type
  using UniquePtr = std::unique_ptr<Image>;

  //! @name Creators
  //@{
  /*!
    @brief Constructor taking the image type, a bitmap of the supported
        metadata types and an auto-pointer that owns an IO instance.
        See subclass constructor doc.
   */
  Image(ImageType type, uint16_t supportedMetadata, BasicIo::UniquePtr io);
  //! Virtual Destructor
  virtual ~Image() = default;
  //@}

  //! @name Manipulators
  //@{
  /*!
    @brief Print out the structure of image file.
    @throw Error if reading of the file fails or the image data is
          not valid (does not look like data of the specific image type).
    @warning This function is not thread safe and intended for exiv2 -pS for debugging.
    @warning You may need to put the stream into binary mode (see src/actions.cpp)
   */
  virtual void printStructure(std::ostream& out, PrintStructureOption option = kpsNone, size_t depth = 0);
  /*!
    @brief Read all metadata supported by a specific image format from the
        image. Before this method is called, the image metadata will be
        cleared.

    This method returns success even if no metadata is found in the
    image. Callers must therefore check the size of individual metadata
    types before accessing the data.

    @throw Error if opening or reading of the file fails or the image
        data is not valid (does not look like data of the specific image
        type).
   */
  virtual void readMetadata() = 0;
  /*!
    @brief Write metadata back to the image.

    All existing metadata sections in the image are either created,
    replaced, or erased. If values for a given metadata type have been
    assigned, a section for that metadata type will either be created or
    replaced. If no values have been assigned to a given metadata type,
    any exists section for that metadata type will be removed from the
    image.

    @throw Error if the operation fails
   */
  virtual void writeMetadata() = 0;
  /*!
    @brief Assign new Exif data. The new Exif data is not written
        to the image until the writeMetadata() method is called.
    @param exifData An ExifData instance holding Exif data to be copied
   */
  virtual void setExifData(const ExifData& exifData);
  /*!
    @brief Erase any buffered Exif data. Exif data is not removed from
        the actual image until the writeMetadata() method is called.
   */
  virtual void clearExifData();
  /*!
    @brief Assign new IPTC data. The new IPTC data is not written
        to the image until the writeMetadata() method is called.
    @param iptcData An IptcData instance holding IPTC data to be copied
   */
  virtual void setIptcData(const IptcData& iptcData);
  /*!
    @brief Erase any buffered IPTC data. IPTC data is not removed from
        the actual image until the writeMetadata() method is called.
   */
  virtual void clearIptcData();
  /*!
    @brief Assign a raw XMP packet. The new XMP packet is not written
        to the image until the writeMetadata() method is called.

    Subsequent calls to writeMetadata() write the XMP packet from
    the buffered raw XMP packet rather than from buffered parsed XMP
    data. In order to write from parsed XMP data again, use
    either writeXmpFromPacket(false) or setXmpData().

    @param xmpPacket A string containing the raw XMP packet.
   */
  virtual void setXmpPacket(const std::string& xmpPacket);
  /*!
    @brief Erase the buffered XMP packet. XMP data is not removed from
        the actual image until the writeMetadata() method is called.

    This has the same effect as clearXmpData() but operates on the
    buffered raw XMP packet only, not the parsed XMP data.

    Subsequent calls to writeMetadata() write the XMP packet from
    the buffered raw XMP packet rather than from buffered parsed XMP
    data. In order to write from parsed XMP data again, use
    either writeXmpFromPacket(false) or setXmpData().
   */
  virtual void clearXmpPacket();
  /*!
    @brief Assign new XMP data. The new XMP data is not written
        to the image until the writeMetadata() method is called.

    Subsequent calls to writeMetadata() encode the XMP data to
    a raw XMP packet and write the newly encoded packet to the image.
    In the process, the buffered raw XMP packet is updated.
    In order to write directly from the raw XMP packet, use
    writeXmpFromPacket(true) or setXmpPacket().

    @param xmpData An XmpData instance holding XMP data to be copied
   */
  virtual void setXmpData(const XmpData& xmpData);
  /*!
    @brief Erase any buffered XMP data. XMP data is not removed from
        the actual image until the writeMetadata() method is called.

    This has the same effect as clearXmpPacket() but operates on the
    buffered parsed XMP data.

    Subsequent calls to writeMetadata() encode the XMP data to
    a raw XMP packet and write the newly encoded packet to the image.
    In the process, the buffered raw XMP packet is updated.
    In order to write directly from the raw XMP packet, use
    writeXmpFromPacket(true) or setXmpPacket().
   */
  virtual void clearXmpData();

  /// @brief Set the image comment. The comment is written to the image when writeMetadata() is called.
  virtual void setComment(const std::string& comment);

  /*!
    @brief Erase any buffered comment. Comment is not removed
        from the actual image until the writeMetadata() method is called.
   */
  virtual void clearComment();
  /*!
    @brief Set the image iccProfile. The new profile is not written
        to the image until the writeMetadata() method is called.
    @param iccProfile DataBuf containing profile (binary)
    @param bTestValid - tests that iccProfile contains credible data
   */
  virtual void setIccProfile(DataBuf&& iccProfile, bool bTestValid = true);
  /*!
    @brief Append more bytes to the iccProfile.
    @param bytes array of bytes to append
    @param size number of bytes to append
    @param bTestValid - tests that iccProfile contains credible data
   */
  void appendIccProfile(const uint8_t* bytes, size_t size, bool bTestValid);
  /*!
    @brief Throw an exception if the size at the beginning of the iccProfile isn't correct.
   */
  void checkIccProfile();
  /*!
    @brief Erase iccProfile. the profile is not removed from
        the actual image until the writeMetadata() method is called.
   */
  virtual void clearIccProfile();
  /*!
    @brief Returns the status of the ICC profile in the image instance
   */
  [[nodiscard]] virtual bool iccProfileDefined() const {
    return !iccProfile_.empty();
  }

  /*!
    @brief return iccProfile
   */
  [[nodiscard]] virtual const DataBuf& iccProfile() const {
    return iccProfile_;
  }

  /*!
    @brief Copy all existing metadata from source Image. The data is
        copied into internal buffers and is not written to the image
        until the writeMetadata() method is called.
    @param image Metadata source. All metadata types are copied.
   */
  virtual void setMetadata(const Image& image);
  /*!
    @brief Erase all buffered metadata. Metadata is not removed
        from the actual image until the writeMetadata() method is called.
   */
  virtual void clearMetadata();
  /*!
    @brief Returns an ExifData instance containing currently buffered
        Exif data.

    The contained Exif data may have been read from the image by
    a previous call to readMetadata() or added directly. The Exif
    data in the returned instance will be written to the image when
    writeMetadata() is called.

    @return modifiable ExifData instance containing Exif values
   */
  virtual ExifData& exifData();
  /*!
    @brief Returns an IptcData instance containing currently buffered
        IPTC data.

    The contained IPTC data may have been read from the image by
    a previous call to readMetadata() or added directly. The IPTC
    data in the returned instance will be written to the image when
    writeMetadata() is called.

    @return modifiable IptcData instance containing IPTC values
   */
  virtual IptcData& iptcData();
  /*!
    @brief Returns an XmpData instance containing currently buffered
        XMP data.

    The contained XMP data may have been read from the image by
    a previous call to readMetadata() or added directly. The XMP
    data in the returned instance will be written to the image when
    writeMetadata() is called.

    @return modifiable XmpData instance containing XMP values
   */
  virtual XmpData& xmpData();
  /*!
    @brief Return a modifiable reference to the raw XMP packet.
   */
  virtual std::string& xmpPacket();
  /*!
    @brief Determine the source when writing XMP.

    Depending on the setting of this flag, writeMetadata() writes
    XMP from the buffered raw XMP packet or from parsed XMP data.
    The default is to write from parsed XMP data. The switch is also
    set by all functions to set and clear the buffered raw XMP packet
    and parsed XMP data, so using this function should usually not be
    necessary.

    If %Exiv2 was compiled without XMP support, the default for this
    flag is true and it will never be changed in order to preserve
    access to the raw XMP packet.
   */
  void writeXmpFromPacket(bool flag);
  /*!
    @brief Set the byte order to encode the Exif metadata in.

    The setting is only used when new Exif metadata is created and may
    not be applicable at all for some image formats. If the target image
    already contains Exif metadata, the byte order of the existing data
    is used. If byte order is not set when writeMetadata() is called,
    little-endian byte order (II) is used by default.
   */
  void setByteOrder(ByteOrder byteOrder);

  /*!
    @brief Print out the structure of image file.
    @throw Error if reading of the file fails or the image data is
          not valid (does not look like data of the specific image type).
   */
  void printTiffStructure(BasicIo& io, std::ostream& out, PrintStructureOption option, size_t depth, size_t offset = 0);

  /*!
    @brief Print out the structure of a TIFF IFD
   */
  void printIFDStructure(BasicIo& io, std::ostream& out, Exiv2::PrintStructureOption option, size_t start, bool bSwap,
                         char c, size_t depth);

  /*!
    @brief is the host platform bigEndian
   */
  static bool isBigEndianPlatform();

  /*!
    @brief is the host platform littleEndian
   */
  static bool isLittleEndianPlatform();

  static bool isStringType(uint16_t type);
  static bool isShortType(uint16_t type);
  static bool isLongType(uint16_t type);
  static bool isLongLongType(uint16_t type);
  static bool isRationalType(uint16_t type);
  static bool is2ByteType(uint16_t type);
  static bool is4ByteType(uint16_t type);
  static bool is8ByteType(uint16_t type);
  static bool isPrintXMP(uint16_t type, Exiv2::PrintStructureOption option);
  static bool isPrintICC(uint16_t type, Exiv2::PrintStructureOption option);

  static uint64_t byteSwap(uint64_t value, bool bSwap);
  static uint32_t byteSwap(uint32_t value, bool bSwap);
  static uint16_t byteSwap(uint16_t value, bool bSwap);
  static uint16_t byteSwap2(const DataBuf& buf, size_t offset, bool bSwap);
  static uint32_t byteSwap4(const DataBuf& buf, size_t offset, bool bSwap);
  static uint64_t byteSwap8(const DataBuf& buf, size_t offset, bool bSwap);

  //@}

  //! @name Accessors
  //@{
  /*!
    @brief Return the byte order in which the Exif metadata of the image is
           encoded. Initially, it is not set (\em invalidByteOrder).
   */
  [[nodiscard]] ByteOrder byteOrder() const;

  /*! @brief Check if the Image instance is valid. Use after object construction.
    @return true if the Image is in a valid state.
   */
  [[nodiscard]] bool good() const;
  /*!
    @brief Return the MIME type of the image.

    @note For each supported image format, the library knows only one MIME
    type.  This may not be the most specific MIME type for that format. In
    particular, several RAW formats are variants of the TIFF format with
    the same magic as TIFF itself. Class TiffImage handles most of them
    and thus they all have MIME type "image/tiff", although a more
    specific MIME type may exist (e.g., "image/x-nikon-nef").
   */
  [[nodiscard]] virtual std::string mimeType() const = 0;
  /*!
    @brief Return the pixel width of the image.
   */
  [[nodiscard]] virtual uint32_t pixelWidth() const;
  /*!
    @brief Return the pixel height of the image.
   */
  [[nodiscard]] virtual uint32_t pixelHeight() const;
  /*!
    @brief Returns an ExifData instance containing currently buffered
        Exif data.

    The Exif data may have been read from the image by
    a previous call to readMetadata() or added directly. The Exif
    data in the returned instance will be written to the image when
    writeMetadata() is called.

    @return read only ExifData instance containing Exif values
   */
  [[nodiscard]] virtual const ExifData& exifData() const;
  /*!
    @brief Returns an IptcData instance containing currently buffered
        IPTC data.

    The contained IPTC data may have been read from the image by
    a previous call to readMetadata() or added directly. The IPTC
    data in the returned instance will be written to the image when
    writeMetadata() is called.

    @return modifiable IptcData instance containing IPTC values
   */
  [[nodiscard]] virtual const IptcData& iptcData() const;
  /*!
    @brief Returns an XmpData instance containing currently buffered
        XMP data.

    The contained XMP data may have been read from the image by
    a previous call to readMetadata() or added directly. The XMP
    data in the returned instance will be written to the image when
    writeMetadata() is called.

    @return modifiable XmpData instance containing XMP values
   */
  [[nodiscard]] virtual const XmpData& xmpData() const;
  /*!
    @brief Return a copy of the image comment. May be an empty string.
   */
  [[nodiscard]] virtual std::string comment() const;
  /*!
    @brief Return the raw XMP packet as a string.
   */
  [[nodiscard]] virtual const std::string& xmpPacket() const;
  /*!
    @brief Return a reference to the BasicIo instance being used for Io.

    This reference is particularly useful to reading the results of
    operations on a MemIo instance. For example after metadata has
    been modified and the writeMetadata() method has been called,
    this method can be used to get access to the modified image.

    @return BasicIo instance that can be used to read or write image
       data directly.
    @note If the returned BasicIo is used to write to the image, the
       Image class will not see those changes until the readMetadata()
       method is called.
   */
  [[nodiscard]] virtual BasicIo& io() const;
  /*!
    @brief Returns the access mode, i.e., the metadata functions, which
       this image supports for the metadata type \em metadataId.
    @param metadataId The metadata identifier.
    @return Access mode for the requested image type and metadata identifier.
   */
  [[nodiscard]] AccessMode checkMode(MetadataId metadataId) const;
  /*!
    @brief Check if image supports a particular type of metadata.
       This method is deprecated. Use checkMode() instead.
   */
  [[nodiscard]] bool supportsMetadata(MetadataId metadataId) const;
  //! Return the flag indicating the source when writing XMP metadata.
  [[nodiscard]] bool writeXmpFromPacket() const;
  //! Return list of native previews. This is meant to be used only by the PreviewManager.
  [[nodiscard]] const NativePreviewList& nativePreviews() const;
  //@}

  //! set type support for this image format
  void setTypeSupported(ImageType imageType, uint16_t supportedMetadata) {
    imageType_ = imageType;
    supportedMetadata_ = supportedMetadata;
  }

  //! set type support for this image format
  [[nodiscard]] ImageType imageType() const {
    return imageType_;
  }

  //! @name NOT implemented
  //@{
  //! Copy constructor
  Image(const Image&) = delete;
  //! Assignment operator
  Image& operator=(const Image&) = delete;
  //@}

 protected:
  // DATA
  BasicIo::UniquePtr io_;             //!< Image data IO pointer
  ExifData exifData_;                 //!< Exif data container
  IptcData iptcData_;                 //!< IPTC data container
  XmpData xmpData_;                   //!< XMP data container
  DataBuf iccProfile_;                //!< ICC buffer (binary data)
  std::string comment_;               //!< User comment
  std::string xmpPacket_;             //!< XMP packet
  uint32_t pixelWidth_{0};            //!< image pixel width
  uint32_t pixelHeight_{0};           //!< image pixel height
  NativePreviewList nativePreviews_;  //!< list of native previews

  //! Return tag name for given tag id.
  const std::string& tagName(uint16_t tag);

  //! Return tag type for given tag id.
  static const char* typeName(uint16_t tag);

 private:
  // DATA
  ImageType imageType_;         //!< Image type
  uint16_t supportedMetadata_;  //!< Bitmap with all supported metadata types
#ifdef EXV_HAVE_XMP_TOOLKIT
  bool writeXmpFromPacket_{false};  //!< Determines the source when writing XMP
#else
  bool writeXmpFromPacket_{true};  //!< Determines the source when writing XMP
#endif
  ByteOrder byteOrder_{invalidByteOrder};  //!< Byte order

  std::map<int, std::string> tags_;  //!< Map of tags
  bool init_{true};                  //!< Flag marking if map of tags needs to be initialized

};  // class Image

//! Type for function pointer that creates new Image instances
using NewInstanceFct = Image::UniquePtr (*)(BasicIo::UniquePtr io, bool create);
//! Type for function pointer that checks image types
using IsThisTypeFct = bool (*)(BasicIo& iIo, bool advance);

/*!
  @brief Returns an Image instance of the specified type.

  The factory is implemented as a static class.
*/
class EXIV2API ImageFactory {
  friend bool Image::good() const;

 public:
  /*!
    @brief Create the appropriate class type implemented BasicIo based on the protocol of the input.

    "-" path implies the data from stdin and it is handled by StdinIo.
    Http path can be handled by either HttpIo or CurlIo. Https, ftp paths
    are handled by CurlIo. Ssh, sftp paths are handled by SshIo. Others are handled by FileIo.

    @param path %Image file.
    @param useCurl Indicate whether the libcurl is used or not.
          If it's true, http is handled by CurlIo. Otherwise it is handled by HttpIo.
    @return An auto-pointer that owns an BasicIo instance.
    @throw Error If the file is not found or it is unable to connect to the server to
          read the remote file.
   */
  static BasicIo::UniquePtr createIo(const std::string& path, bool useCurl = true);

  /*!
    @brief Create an Image subclass of the appropriate type by reading
        the specified file. %Image type is derived from the file
        contents.
    @param  path %Image file. The contents of the file are tested to
        determine the image type. File extension is ignored.
    @param useCurl Indicate whether the libcurl is used or not.
          If it's true, http is handled by CurlIo. Otherwise it is handled by HttpIo.
    @return An auto-pointer that owns an Image instance whose type
        matches that of the file.
    @throw Error If opening the file fails or it contains data of an
        unknown image type.
   */
  static Image::UniquePtr open(const std::string& path, bool useCurl = true);

  /*!
    @brief Create an Image subclass of the appropriate type by reading
        the provided memory. %Image type is derived from the memory
        contents.
    @param data Pointer to a data buffer containing an image. The contents
        of the memory are tested to determine the image type.
    @param size Number of bytes pointed to by \em data.
    @return An auto-pointer that owns an Image instance whose type
        matches that of the data buffer.
    @throw Error If the memory contains data of an unknown image type.
   */
  static Image::UniquePtr open(const byte* data, size_t size);
  /*!
    @brief Create an Image subclass of the appropriate type by reading
        the provided BasicIo instance. %Image type is derived from the
        data provided by \em io. The passed in \em io instance is
        (re)opened by this method.
    @param io An auto-pointer that owns a BasicIo instance that provides
        image data. The contents of the image data are tested to determine
        the type.
    @note This method takes ownership of the passed
        in BasicIo instance through the auto-pointer. Callers should not
        continue to use the BasicIo instance after it is passed to this method.
        Use the Image::io() method to get a temporary reference.
    @return An auto-pointer that owns an Image instance whose type
        matches that of the \em io data. If no image type could be
        determined, the pointer is 0.
    @throw Error If opening the BasicIo fails
   */
  static Image::UniquePtr open(BasicIo::UniquePtr io);
  /*!
    @brief Create an Image subclass of the requested type by creating a
        new image file. If the file already exists, it will be overwritten.
    @param type Type of the image to be created.
    @param path %Image file to create. File extension is ignored.
    @return An auto-pointer that owns an Image instance of the requested
        type.
    @throw Error If the image type is not supported.
   */
  static Image::UniquePtr create(ImageType type, const std::string& path);
  /*!
    @brief Create an Image subclass of the requested type by creating a
        new image in memory.
    @param type Type of the image to be created.
    @return An auto-pointer that owns an Image instance of the requested
        type.
    @throw Error If the image type is not supported
   */
  static Image::UniquePtr create(ImageType type);

  /*!
    @brief Create an Image subclass of the requested type by writing a
        new image to a BasicIo instance. If the BasicIo instance already
        contains data, it will be overwritten.
    @param type Type of the image to be created.
    @param io An auto-pointer that owns a BasicIo instance that will
        be written to when creating a new image.
    @note This method takes ownership of the passed in BasicIo instance
        through the auto-pointer. Callers should not continue to use the
        BasicIo instance after it is passed to this method.  Use the
        Image::io() method to get a temporary reference.
    @return An auto-pointer that owns an Image instance of the requested
        type. If the image type is not supported, the pointer is 0.
   */

  static Image::UniquePtr create(ImageType type, BasicIo::UniquePtr io);
  /*!
    @brief Returns the image type of the provided file.
    @param path %Image file. The contents of the file are tested to
        determine the image type. File extension is ignored.
    @return %Image type or Image::none if the type is not recognized.
   */
  static ImageType getType(const std::string& path);
  /*!
    @brief Returns the image type of the provided data buffer.
    @param data Pointer to a data buffer containing an image. The contents
        of the memory are tested to determine the image type.
    @param size Number of bytes pointed to by \em data.
    @return %Image type or Image::none if the type is not recognized.
   */
  static ImageType getType(const byte* data, size_t size);
  /*!
    @brief Returns the image type of data provided by a BasicIo instance.
        The passed in \em io instance is (re)opened by this method.
    @param io A BasicIo instance that provides image data. The contents
        of the image data are tested to determine the type.
    @return %Image type or Image::none if the type is not recognized.
   */
  static ImageType getType(BasicIo& io);
  /*!
    @brief Returns the access mode or supported metadata functions for an
        image type and a metadata type.
    @param type       The image type.
    @param metadataId The metadata identifier.
    @return Access mode for the requested image type and metadata identifier.
    @throw Error(kerUnsupportedImageType) if the image type is not supported.
   */
  static AccessMode checkMode(ImageType type, MetadataId metadataId);
  /*!
    @brief Determine if the content of \em io is an image of \em type.

    The \em advance flag determines if the read position in the
    stream is moved (see below). This applies only if the type
    matches and the function returns true. If the type does not
    match, the stream position is not changed. However, if
    reading from the stream fails, the stream position is
    undefined. Consult the stream state to obtain more
    information in this case.

    @param type Type of the image.
    @param io BasicIo instance to read from.
    @param advance Flag indicating whether the position of the io
        should be advanced by the number of characters read to
        analyse the data (true) or left at its original
        position (false). This applies only if the type matches.
    @return  true  if the data matches the type of this class;<BR>
             false if the data does not match
  */
  static bool checkType(ImageType type, BasicIo& io, bool advance);
};  // class ImageFactory

// *****************************************************************************
// template, inline and free functions

//! Append \em len bytes pointed to by \em buf to \em blob.
EXIV2API void append(Exiv2::Blob& blob, const byte* buf, size_t len);

}  // namespace Exiv2

#endif  // #ifndef IMAGE_HPP_
