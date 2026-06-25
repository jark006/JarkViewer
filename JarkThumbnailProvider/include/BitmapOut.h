#pragma once

#include "ThumbTypes.h"

#include <thumbcache.h>
#include <windows.h>

namespace JarkThumbnail {
HRESULT createThumbnailBitmap(const ThumbBitmap& source, UINT maxEdge, HBITMAP* bitmap, WTS_ALPHATYPE* alphaType) noexcept;
}
