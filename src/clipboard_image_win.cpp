#include "clipboard_image.h"
#include "render.h"
#include "text.h"
#include <stdexcept>

namespace poppy {
namespace {
UINT pngFormat() { static const UINT format = RegisterClipboardFormatW(L"PNG"); return format; }
void checked(HRESULT result) {
    if (FAILED(result)) throw std::runtime_error("Cannot convert the clipboard image to PNG (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ").");
}
}
bool clipboardHasImage() {
    const auto png = pngFormat();
    return (png && IsClipboardFormatAvailable(png)) || IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB);
}
ClipboardImage readClipboardImage(HWND owner) {
    if (!OpenClipboard(owner)) throw std::runtime_error("Cannot open the clipboard. Try again.");
    struct Close { ~Close() { CloseClipboard(); } } close;
    const auto png = pngFormat();
    for (const auto format : {png, static_cast<UINT>(CF_DIBV5), static_cast<UINT>(CF_DIB)}) {
        if (!format || !IsClipboardFormatAvailable(format)) continue;
        const auto data = GetClipboardData(format); if (!data) continue;
        const auto size = GlobalSize(data);
        if (!size || size > maxClipboardImageBytes) throw std::runtime_error("Clipboard image is empty or exceeds the 64 MiB limit.");
        const auto bytes = static_cast<const char*>(GlobalLock(data));
        if (!bytes) throw std::runtime_error("Cannot read the clipboard image.");
        struct Unlock { HGLOBAL data; ~Unlock() { GlobalUnlock(data); } } unlock{data};
        return {std::string(bytes, size), format != png};
    }
    throw std::runtime_error("The clipboard does not contain a supported image.");
}
std::string encodeClipboardPng(const ClipboardImage& image) {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) checked(com);
    struct Uninitialize { bool initialized; ~Uninitialize() { if (initialized) CoUninitialize(); } } uninitialize{SUCCEEDED(com)};
    auto bytes = image.dib ? clipboardDibBitmap(image.bytes) : image.bytes;
    if (bytes.empty() || bytes.size() > maxClipboardImageBytes + 14) throw std::runtime_error("Clipboard image is empty or too large.");
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(factory.put())));
    ComPtr<IWICStream> input; checked(factory->CreateStream(input.put()));
    checked(input->InitializeFromMemory(reinterpret_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    checked(factory->CreateDecoderFromStream(input.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));
    ComPtr<IWICBitmapFrameDecode> source; checked(decoder->GetFrame(0, source.put()));
    UINT width = 0, height = 0; checked(source->GetSize(&width, &height));
    if (!width || !height || width > 16384 || height > 16384 || static_cast<uint64_t>(width) * height * 4 > maxClipboardImageBytes)
        throw std::runtime_error("Clipboard image dimensions exceed the 64 MiB pixel limit.");
    ComPtr<IStream> output; checked(CreateStreamOnHGlobal(nullptr, TRUE, output.put()));
    ComPtr<IWICBitmapEncoder> encoder; checked(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    checked(encoder->Initialize(output.get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; checked(encoder->CreateNewFrame(frame.put(), nullptr));
    checked(frame->Initialize(nullptr)); checked(frame->SetSize(width, height));
    auto format = GUID_WICPixelFormat32bppBGRA; checked(frame->SetPixelFormat(&format));
    ComPtr<IWICFormatConverter> converter; checked(factory->CreateFormatConverter(converter.put()));
    checked(converter->Initialize(source.get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    checked(frame->WriteSource(converter.get(), nullptr)); checked(frame->Commit()); checked(encoder->Commit());
    STATSTG info{}; checked(output->Stat(&info, STATFLAG_NONAME));
    if (!info.cbSize.QuadPart || info.cbSize.QuadPart > maxFileBytes) throw std::runtime_error("PNG exceeds the 16 MiB file limit.");
    std::string result(static_cast<size_t>(info.cbSize.QuadPart), '\0');
    LARGE_INTEGER start{}; checked(output->Seek(start, STREAM_SEEK_SET, nullptr));
    ULONG count = 0; checked(output->Read(result.data(), static_cast<ULONG>(result.size()), &count));
    if (count != result.size()) throw std::runtime_error("Cannot read the encoded PNG.");
    return result;
}
}
