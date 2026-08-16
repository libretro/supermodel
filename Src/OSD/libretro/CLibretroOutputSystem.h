#ifndef CLIBRETROOUTPUTSYSTEM_H
#define CLIBRETROOUTPUTSYSTEM_H

#include "OSD/Outputs.h"

class CLibretroOutputSystem : public COutputs
{
public:
    CLibretroOutputSystem();
    virtual ~CLibretroOutputSystem();

    // You must declare these because they are pure virtual in COutputs
    virtual bool Initialize() override;
    virtual void Attached() override;

protected:
    virtual void SendOutput(EOutputs output, UINT8 prevValue, UINT8 value) override;
};

#endif