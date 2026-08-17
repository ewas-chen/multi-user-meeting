#include "AudioCodecFactory.h"

#include "OpusCodec.h"

namespace TRANSPORT {
namespace CODEC {

std::unique_ptr<IAudioEncoder>
AudioCodecFactory::CreateEncoder(AudioCodecType type)
{
    switch (type) {
        case AudioCodecType::kOpus:
            return std::make_unique<OpusCodecEncoder>();
    }

    return nullptr;
}

std::unique_ptr<IAudioDecoder>
AudioCodecFactory::CreateDecoder(AudioCodecType type)
{
    switch (type) {
        case AudioCodecType::kOpus:
            return std::make_unique<OpusCodecDecoder>();
    }

    return nullptr;
}

} // namespace CODEC
} // namespace TRANSPORT