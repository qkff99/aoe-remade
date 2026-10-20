#include "stdafx.h"
#include "profiler.h"

#if XRCORE_PROFILER == PROFILER_OPTICK
#include "../3rd party/optick-git/src/optick_core.h"

void xrOptickEnableThreadTags()
{
    // Optick's late RegisterThread binds TLS but leaves a new storage in OFF mode.
    // Events still record; tags require the capture's TAGS bit on this storage.
    auto* storage = *Optick::GetEventStorageSlotForCurrentThread();
    if (storage && Optick::IsActive(Optick::Mode::TAGS))
        storage->currentMode = Optick::Mode::Type(storage->currentMode | Optick::Mode::TAGS);
}
#endif
