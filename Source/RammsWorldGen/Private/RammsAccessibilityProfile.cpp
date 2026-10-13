// Copyright ATDev. All Rights Reserved.
#include "RammsAccessibilityProfile.h"

void URammsAccessibilityProfile::ApplyTo(FRammsWorldGenParams& Params) const
{
	Params.MinClearWidth = MinClearWidth;
	Params.PreferredClearWidth = PreferredClearWidth;
	Params.LandingSize = LandingSize;
	Params.VerticalChangeMax = VerticalChangeMax;
	Params.RampMaxSlope = RampMaxSlope;
	Params.MaxCrossSlope = MaxCrossSlope;
	Params.CurbRampFlareSlope = CurbRampFlareSlope;
	Params.DetectableWarningDepth = DetectableWarningDepth;
	Params.DoorClearWidth = DoorClearWidth;
	Params.DoorThresholdMax = DoorThresholdMax;
}
