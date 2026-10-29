#pragma once

#include "TPS53647.h"

class TPS53667 : public TPS53647
{
public:
    static const uint16_t DEVICE_CODE = 0x51;
    TPS53667();
    virtual bool init(int num_phases, int imax, float ifault) override;
    virtual void status() override;
    virtual void set_phases(int num_phases) override;
};
