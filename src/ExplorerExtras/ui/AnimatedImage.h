// AnimatedImage.h - a GIF, playing rather than sitting still.
//
// A shell thumbnail of an animated GIF is one frame of it, and usually the
// least interesting one: the first frame of an animation is often blank, a
// title card, or a single line of a drawing that only makes sense once it
// moves. Nothing registers a preview handler for image/gif either, so without
// this a GIF previews as a still.
//
// Decoding is GDI+ rather than WIC because GIF frames are not whole pictures:
// most are a patch of changed pixels plus a disposal rule saying what to do
// with the one underneath. GDI+ applies all that and hands back composed
// frames; with WIC the compositing would have to be written here.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace Gdiplus {
class Image;
}

namespace ee {

class AnimatedImage {
public:
    // Both defined where Gdiplus::Image is a complete type rather than here:
    // a unique_ptr to a forward-declared class cannot be destroyed - or
    // unwound out of a constructor - without one.
    AnimatedImage();
    ~AnimatedImage();

    AnimatedImage(const AnimatedImage&) = delete;
    AnimatedImage& operator=(const AnimatedImage&) = delete;
    AnimatedImage(AnimatedImage&&) noexcept;
    AnimatedImage& operator=(AnimatedImage&&) noexcept;

    // False for anything that will not animate - a still image, a GIF of one
    // frame, a file GDI+ cannot read - so the caller can fall back to the
    // thumbnail it would otherwise have shown.
    bool Open(const std::wstring& path);
    void Close();

    bool IsOpen() const { return image_ != nullptr; }
    int Frames() const { return frames_; }
    SIZE Size() const { return size_; }

    // How long frame |index| is meant to stay up.
    UINT DelayMs(int index) const;

    // Frame |index|, scaled to |target| and composed over |background|, since
    // a GIF's transparent pixels have to become something. Caller owns the
    // bitmap.
    HBITMAP RenderFrame(int index, SIZE target, COLORREF background);

    // GDI+ stays started for as long as the process previews anything; this
    // ends it, and is for shutdown on the thread that did the previewing.
    static void ShutdownDecoder();

private:
    std::unique_ptr<Gdiplus::Image> image_;
    std::vector<UINT> delays_;  // milliseconds, one per frame
    int frames_ = 0;
    SIZE size_{};
};

// True for a path worth trying, on the extension alone. Cheap enough to run on
// every hover, which is the point: opening the file is not.
bool IsAnimatablePath(const std::wstring& path);

}  // namespace ee
