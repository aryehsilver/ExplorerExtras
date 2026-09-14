#include "AnimatedImage.h"

#include <objidl.h>  // gdiplus.h is written against IStream
#include <shlwapi.h>

#include <algorithm>

// The project builds with NOMINMAX, and gdiplus.h uses min and max unqualified.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus

#include <gdiplus.h>

#include "../core/Logging.h"

namespace ee {
namespace {

// A GIF that asks for no delay at all, or an implausibly short one, is asking
// to run as fast as the machine can draw it. Every browser quietly reads that
// as a tenth of a second instead, and so does this.
constexpr UINT kMinimumDelayMs = 20;
constexpr UINT kDefaultDelayMs = 100;

ULONG_PTR g_gdiplus_token = 0;

bool EnsureDecoder() {
    if (g_gdiplus_token != 0) return true;

    Gdiplus::GdiplusStartupInput input;
    const Gdiplus::Status status = Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr);
    if (status != Gdiplus::Ok) {
        EE_ERR(L"GdiplusStartup failed, status=%d", static_cast<int>(status));
        g_gdiplus_token = 0;
        return false;
    }
    return true;
}

}  // namespace

bool IsAnimatablePath(const std::wstring& path) {
    if (path.empty()) return false;
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    return CompareStringOrdinal(extension, -1, L".gif", -1, TRUE) == CSTR_EQUAL;
}

AnimatedImage::AnimatedImage() = default;

AnimatedImage::~AnimatedImage() {
    Close();
}

AnimatedImage::AnimatedImage(AnimatedImage&& other) noexcept
    : image_(std::move(other.image_)),
      delays_(std::move(other.delays_)),
      frames_(other.frames_),
      size_(other.size_) {
    other.frames_ = 0;
    other.size_ = SIZE{};
}

AnimatedImage& AnimatedImage::operator=(AnimatedImage&& other) noexcept {
    if (this != &other) {
        Close();
        image_ = std::move(other.image_);
        delays_ = std::move(other.delays_);
        frames_ = other.frames_;
        size_ = other.size_;
        other.frames_ = 0;
        other.size_ = SIZE{};
    }
    return *this;
}

void AnimatedImage::Close() {
    image_.reset();
    delays_.clear();
    frames_ = 0;
    size_ = SIZE{};
}

void AnimatedImage::ShutdownDecoder() {
    if (g_gdiplus_token == 0) return;
    Gdiplus::GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
}

bool AnimatedImage::Open(const std::wstring& path) {
    Close();
    if (path.empty() || !EnsureDecoder()) return false;

    auto image = std::make_unique<Gdiplus::Image>(path.c_str());
    if (image->GetLastStatus() != Gdiplus::Ok) return false;

    const UINT frames = image->GetFrameCount(&Gdiplus::FrameDimensionTime);
    if (frames <= 1) return false;  // a still: the thumbnail path renders it better

    // Every frame's delay arrives in one property, as hundredths of a second.
    std::vector<UINT> delays;
    const UINT property_size = image->GetPropertyItemSize(PropertyTagFrameDelay);
    if (property_size > 0) {
        std::vector<BYTE> buffer(property_size);
        auto* item = reinterpret_cast<Gdiplus::PropertyItem*>(buffer.data());
        if (image->GetPropertyItem(PropertyTagFrameDelay, property_size, item) == Gdiplus::Ok &&
            item->value != nullptr) {
            const auto* values = static_cast<const UINT*>(item->value);
            const UINT count = item->length / sizeof(UINT);
            for (UINT i = 0; i < count; ++i) delays.push_back(values[i] * 10);
        }
    }
    delays.resize(frames, 0);

    image_ = std::move(image);
    delays_ = std::move(delays);
    frames_ = static_cast<int>(frames);
    size_ = SIZE{static_cast<LONG>(image_->GetWidth()), static_cast<LONG>(image_->GetHeight())};
    if (size_.cx <= 0 || size_.cy <= 0) {
        Close();
        return false;
    }
    return true;
}

UINT AnimatedImage::DelayMs(int index) const {
    if (index < 0 || index >= static_cast<int>(delays_.size())) return kDefaultDelayMs;
    const UINT delay = delays_[index];
    return delay < kMinimumDelayMs ? kDefaultDelayMs : delay;
}

HBITMAP AnimatedImage::RenderFrame(int index, SIZE target, COLORREF background) {
    if (!image_ || index < 0 || index >= frames_ || target.cx <= 0 || target.cy <= 0) {
        return nullptr;
    }
    if (image_->SelectActiveFrame(&Gdiplus::FrameDimensionTime, static_cast<UINT>(index)) !=
        Gdiplus::Ok) {
        return nullptr;
    }

    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, target.cx, target.cy);
    ReleaseDC(nullptr, screen);
    if (!memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        return nullptr;
    }

    const HGDIOBJ previous = SelectObject(memory, bitmap);
    {
        // Scoped: the Graphics has to be gone before the bitmap leaves the DC.
        Gdiplus::Graphics graphics(memory);
        // A GIF's transparent pixels have to become something, and the preview's
        // own background is the only answer that does not show as a box.
        graphics.Clear(Gdiplus::Color(255, GetRValue(background), GetGValue(background),
                                      GetBValue(background)));
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.DrawImage(image_.get(), 0, 0, target.cx, target.cy);
    }
    SelectObject(memory, previous);
    DeleteDC(memory);
    return bitmap;
}

}  // namespace ee
