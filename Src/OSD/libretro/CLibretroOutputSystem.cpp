#include <cstring>
#include <libretro.h>
#include "CLibretroOutputSystem.h"

// Note: These aren't strictly needed for Outputs, but keep them if you plan 
// to use them for dashboard-led logic later.
extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;
extern retro_log_printf_t log_cb;  // defined in libretro.cpp

CLibretroOutputSystem::CLibretroOutputSystem() 
    : COutputs()
{
}

CLibretroOutputSystem::~CLibretroOutputSystem() 
{
}

// Satisfy the linker for the pure virtuals inherited from COutputs
bool CLibretroOutputSystem::Initialize()
{
    // Return true to indicate the output system is "ready"
    return true;
}

void CLibretroOutputSystem::Attached()
{
    // Called when the Model 3 engine successfully attaches this system
}

void CLibretroOutputSystem::SendOutput(EOutputs output, UINT8 prevValue, UINT8 value)
{
    // RawDrive = index 7. This fires on every drive-board output change — many
    // times per frame on force-feedback games (e.g. srally2). The log_cb call
    // (varargs format + frontend level check) runs even when the frontend
    // filters DEBUG out, so it's pure overhead and floods the log. Gate it
    // behind a DEBUG build so retail builds pay nothing.
#ifdef DEBUG
    if (output == OutputRawDrive) {
        log_cb(RETRO_LOG_DEBUG, "[Outputs] RawDrive: %u\n", value);
    }
#else
    (void)output; (void)prevValue; (void)value;
#endif
}