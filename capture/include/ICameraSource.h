
#pragma once

#include "CaptureDefine.h"
#include "ISource.h"
#include <memory>

namespace CAPTURE {
class ICameraSourceProperty;
class CAPTURE_ENGINE_API ICameraSource : public ISource {
public:
    virtual ~ICameraSource() override = default;
    virtual std::shared_ptr<ICameraSourceProperty> GetProperty() const noexcept = 0;
};

}