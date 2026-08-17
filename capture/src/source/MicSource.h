#pragma once

#include "BaseSource.h"
#include "IMicSource.h"
#include "property/MicSourceProperty.h"

namespace CAPTURE {

class MicSource final : public BaseSource<IMicSource, MicSourceProperty> {
public:
    MicSource() = default;
    ~MicSource() override = default;

    std::shared_ptr<IMicSourceProperty> GetProperty() const noexcept override {
        return m_property;
    }
};

} // namespace CAPTURE