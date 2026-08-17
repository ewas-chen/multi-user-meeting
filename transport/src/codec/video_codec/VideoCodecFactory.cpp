#include "VideoCodecFactory.h"

#include "H264Codec.h"

namespace TRANSPORT {
namespace CODEC {

std::unique_ptr<IVideoEncoder>
VideoCodecFactory::CreateEncoder(VideoCodecType type)
{
    switch (type) {
        case VideoCodecType::kH264:
            return std::make_unique<H264Encoder>();
    }

    return nullptr;
}

std::unique_ptr<IVideoDecoder>
VideoCodecFactory::CreateDecoder(VideoCodecType type)
{
    switch (type) {
        case VideoCodecType::kH264:
            return std::make_unique<H264Decoder>();
    }

    return nullptr;
}

} // namespace CODEC
} // namespace TRANSPORT