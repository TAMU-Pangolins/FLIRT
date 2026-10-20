// Stand-ins for the FTDI D3XX functions mesytec-mvlc's USB code calls, compiled
// into libmesytec-mvlc on macOS only (top-level CMakeLists.txt).
//
// mesytec-mvlc has no macOS USB source (src/mesytec-mvlc/CMakeLists.txt:66-81),
// and the D3XX library it ships for macOS is x86_64 only. FLIRT builds the
// unmodified Linux USB source there (mvlc_impl_usb_linux_x86_64.cc), because
// the library references usb::Impl (mvlc_factory.cc:36-48, mvlc_dialog.cc:151).
// FLIRT replays files and never opens an MVLC, so these functions only have to
// say "no device": the device list is empty, FT_Create finds nothing, and every
// call on a handle fails.

#include <ftd3xx.h>

FT_STATUS WINAPI FT_CreateDeviceInfoList(LPDWORD lpdwNumDevs)
{
    if (lpdwNumDevs)
        *lpdwNumDevs = 0;
    return FT_OK;
}

FT_STATUS WINAPI FT_GetDeviceInfoList(FT_DEVICE_LIST_INFO_NODE *, LPDWORD lpdwNumDevs)
{
    if (lpdwNumDevs)
        *lpdwNumDevs = 0;
    return FT_NOT_SUPPORTED;
}

FT_STATUS WINAPI FT_Create(PVOID, DWORD, FT_HANDLE *pftHandle)
{
    if (pftHandle)
        *pftHandle = nullptr;
    return FT_DEVICE_NOT_FOUND;
}

FT_STATUS WINAPI FT_Close(FT_HANDLE)
{
    return FT_INVALID_HANDLE;
}

FT_STATUS WINAPI FT_GetChipConfiguration(FT_HANDLE, PVOID)
{
    return FT_INVALID_HANDLE;
}

FT_STATUS WINAPI FT_SetPipeTimeout(FT_HANDLE, UCHAR, DWORD)
{
    return FT_INVALID_HANDLE;
}

FT_STATUS WINAPI FT_ReadPipe(FT_HANDLE, UCHAR, PUCHAR, ULONG, PULONG pulBytesTransferred, LPOVERLAPPED)
{
    if (pulBytesTransferred)
        *pulBytesTransferred = 0;
    return FT_INVALID_HANDLE;
}

FT_STATUS WINAPI FT_ReadPipeEx(FT_HANDLE, UCHAR, PUCHAR, ULONG, PULONG pulBytesTransferred, DWORD)
{
    if (pulBytesTransferred)
        *pulBytesTransferred = 0;
    return FT_INVALID_HANDLE;
}

FT_STATUS WINAPI FT_WritePipeEx(FT_HANDLE, UCHAR, PUCHAR, ULONG, PULONG pulBytesTransferred, DWORD)
{
    if (pulBytesTransferred)
        *pulBytesTransferred = 0;
    return FT_INVALID_HANDLE;
}
