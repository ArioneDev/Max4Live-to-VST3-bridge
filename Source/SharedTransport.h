#pragma once

#include "SharedProtocol.h"

#include <windows.h>
#include <string>

namespace m4lb
{

// Named shared-memory block, one per bridge instance.  The name embeds the PID
// of the Max process, which is how the two sides find each other without any
// command line plumbing: the shim reads its own PID, the host reads the PID of
// the process it just spawned.
class SharedTransport
{
public:
    SharedTransport() = default;
    ~SharedTransport() { close(); }

    SharedTransport (const SharedTransport&) = delete;
    SharedTransport& operator= (const SharedTransport&) = delete;

    static std::wstring nameForKey (uint32_t key)
    {
        return L"Local\\M4LBridge_" + std::to_wstring (key);
    }

    // Create if missing, otherwise open the existing block.  Returns true when
    // the view is mapped (the header is only valid if createdByUs() or the
    // magic checks out).
    bool attach (uint32_t key)
    {
        close();

        name_ = nameForKey (key);
        map_ = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, (DWORD) kSharedBytes, name_.c_str());
        if (map_ == nullptr)
            return false;

        createdByUs_ = (GetLastError() != ERROR_ALREADY_EXISTS);

        layout_ = static_cast<SharedLayout*> (MapViewOfFile (map_, FILE_MAP_ALL_ACCESS, 0, 0,
                                                             (SIZE_T) kSharedBytes));
        if (layout_ == nullptr)
        {
            close();
            return false;
        }

        if (createdByUs_)
        {
            std::memset (layout_, 0, (size_t) kSharedBytes);
            layout_->header.magic.store (kMagic, std::memory_order_release);
            layout_->header.version.store (kVersion, std::memory_order_release);
        }

        return true;
    }

    void close()
    {
        if (layout_ != nullptr)
        {
            UnmapViewOfFile (layout_);
            layout_ = nullptr;
        }
        if (map_ != nullptr)
        {
            CloseHandle (map_);
            map_ = nullptr;
        }
        createdByUs_ = false;
    }

    bool isAttached() const noexcept { return layout_ != nullptr; }
    bool createdByUs() const noexcept { return createdByUs_; }

    bool valid() const noexcept
    {
        return layout_ != nullptr
            && layout_->header.magic.load (std::memory_order_acquire) == kMagic
            && layout_->header.version.load (std::memory_order_acquire) == kVersion;
    }

    SharedLayout* layout() noexcept { return layout_; }
    const SharedLayout* layout() const noexcept { return layout_; }
    SharedHeader& header() noexcept { return layout_->header; }

private:
    std::wstring name_;
    HANDLE map_ = nullptr;
    SharedLayout* layout_ = nullptr;
    bool createdByUs_ = false;
};

} // namespace m4lb
