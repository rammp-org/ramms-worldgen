// Copyright ATDev. All Rights Reserved.
#include "RammsWorldPreset.h"

#include "RammsAccessibilityProfile.h"

FRammsWorldGenParams URammsWorldPreset::ResolveParams() const
{
	FRammsWorldGenParams Out = Params;
	if (const URammsAccessibilityProfile* P = Profile.LoadSynchronous())
	{
		P->ApplyTo(Out);
	}
	return Out;
}
