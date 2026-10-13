// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Logging/LogMacros.h"

RAMMSWORLDGEN_API DECLARE_LOG_CATEGORY_EXTERN(LogRammsWorldGen, Log, All);

class FRammsWorldGenModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
