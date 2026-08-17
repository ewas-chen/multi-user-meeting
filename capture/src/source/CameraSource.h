#pragma once

#include "BaseSource.h"
#include "ICameraSource.h"
#include "property/CameraSourceProperty.h"

namespace CAPTURE
{

class CameraSource final : public BaseSource<ICameraSource, CameraSourceProperty> {
public:
    CameraSource() = default;
    ~CameraSource() override = default;

    std::shared_ptr<ICameraSourceProperty> GetProperty() const noexcept override {
        return m_property;
    }
};

} // namespace CAPTURE