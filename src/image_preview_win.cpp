#include "image_preview.h"
#include "render.h"
#include "text.h"
#include <stdexcept>

namespace poppy {
namespace {
void checked(HRESULT result) {
    if (FAILED(result)) throw std::runtime_error("Cannot decode this image. The file may be damaged or use an unsupported format (HRESULT " + std::to_string(static_cast<unsigned long>(result)) + ").");
}
}
std::shared_ptr<const ImagePreview> decodeImage(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > maxFileBytes) throw std::runtime_error("Image is empty or exceeds the 16 MiB file limit.");
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) checked(com);
    struct Uninitialize { bool initialized; ~Uninitialize() { if (initialized) CoUninitialize(); } } uninitialize{SUCCEEDED(com)};
    ComPtr<IWICImagingFactory> factory;
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(factory.put())));
    ComPtr<IWICStream> input; checked(factory->CreateStream(input.put()));
    checked(input->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size())));
    ComPtr<IWICBitmapDecoder> decoder;
    checked(factory->CreateDecoderFromStream(input.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));
    ComPtr<IWICBitmapFrameDecode> frame; checked(decoder->GetFrame(0, frame.put()));
    auto image = std::make_shared<ImagePreview>();
    UINT width = 0, height = 0; checked(frame->GetSize(&width, &height));
    const auto size = imagePixelBytes(width, height);
    ComPtr<IWICFormatConverter> converter; checked(factory->CreateFormatConverter(converter.put()));
    checked(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    image->width = width; image->height = height; image->pixels.resize(size);
    checked(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(size), image->pixels.data()));
    return image;
}
}
