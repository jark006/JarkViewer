#pragma once

#include <guiddef.h>
#include <string_view>

namespace JarkThumbnailProviderGuids {
inline constexpr std::wstring_view ProviderClsidString = L"{6677A695-A3CD-4E32-8823-EAD9C293A770}";
inline constexpr std::wstring_view ThumbnailProviderHandlerString = L"{E357FCCD-A995-4576-B01F-234630154E96}";

// {6677A695-A3CD-4E32-8823-EAD9C293A770}
inline constexpr GUID CLSID_JarkThumbnailProvider = {
    0x6677a695,
    0xa3cd,
    0x4e32,
    { 0x88, 0x23, 0xea, 0xd9, 0xc2, 0x93, 0xa7, 0x70 }
};
}
