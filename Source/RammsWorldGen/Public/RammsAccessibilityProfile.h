// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RammsWorldGenTypes.h"
#include "RammsAccessibilityProfile.generated.h"

/** A closed numeric range used to draw deliberately non-compliant dimensions. */
USTRUCT(BlueprintType)
struct RAMMSWORLDGEN_API FRammsRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range") double Min = 0.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Range") double Max = 0.0;

	FRammsRange() = default;
	FRammsRange(double InMin, double InMax) : Min(InMin), Max(InMax) {}
	double Sample(const FRandomStream& Rng) const { return Min + (Max - Min) * static_cast<double>(Rng.FRand()); }
};

/**
 * The dimensional rules a generated world is measured against, and the ranges
 * used when the generator decides to break one of them on purpose.
 *
 * Defaults follow the 2010 ADA Standards for Accessible Design. A world built
 * with NonCompliance = 0 should satisfy every threshold here; with it above 0
 * the generator draws from the non-compliant ranges and records what it broke
 * in the manifest, so an episode knows which features are the hard ones.
 */
UCLASS(BlueprintType)
class RAMMSWORLDGEN_API URammsAccessibilityProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	// --- thresholds (compliant when satisfied) ---------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Routes", meta = (Units = "cm")) double MinClearWidth = 91.4;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Routes", meta = (Units = "cm")) double PreferredClearWidth = 152.4;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Routes", meta = (Units = "cm")) double LandingSize = 152.4;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Routes", meta = (Units = "cm")) double VerticalChangeMax = 0.64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Slopes", meta = (ToolTip = "Rise over run; 0.0833 = 1:12")) double RampMaxSlope = 0.0833;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Slopes") double MaxCrossSlope = 0.02;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Slopes", meta = (ToolTip = "Curb ramp flare slope; 0.1 = 1:10")) double CurbRampFlareSlope = 0.1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Slopes", meta = (Units = "cm", ToolTip = "Maximum rise for a single ramp run before a landing is required")) double RampMaxRise = 76.2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Slopes", meta = (Units = "cm")) double DetectableWarningDepth = 61.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Doors", meta = (Units = "cm")) double DoorClearWidth = 81.3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thresholds|Doors", meta = (Units = "cm")) double DoorThresholdMax = 1.27;

	// --- deliberate non-compliance -----------------------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Non-compliant ranges", meta = (ToolTip = "Clear width left by an intruding hazard, cm")) FRammsRange NarrowClearWidth = FRammsRange(55.0, 88.0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Non-compliant ranges", meta = (ToolTip = "Running slope of a too-steep ramp, rise over run")) FRammsRange SteepSlope = FRammsRange(0.10, 0.20);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Non-compliant ranges", meta = (ToolTip = "Cross slope of a tilted sidewalk segment")) FRammsRange SteepCrossSlope = FRammsRange(0.03, 0.06);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Non-compliant ranges", meta = (ToolTip = "Door leaf width of a too-narrow door, cm")) FRammsRange NarrowDoor = FRammsRange(60.0, 78.0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Non-compliant ranges", meta = (ToolTip = "Height of a raised threshold, cm")) FRammsRange HighThreshold = FRammsRange(2.0, 6.0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Non-compliant ranges", meta = (ToolTip = "Vertical lip of a heaved slab, cm")) FRammsRange SlabLip = FRammsRange(1.5, 5.0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Non-compliant ranges", meta = (ToolTip = "Rise of a single entrance step, cm")) FRammsRange StepRise = FRammsRange(10.0, 18.0);

	/** Copy the thresholds into a parameter block (the generator does this before every generation). */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	void ApplyTo(UPARAM(ref) FRammsWorldGenParams& Params) const;

	bool IsSlopeCompliant(double RiseOverRun) const { return RiseOverRun <= RampMaxSlope + KINDA_SMALL_NUMBER; }
	bool IsClearWidthCompliant(double Width) const { return Width >= MinClearWidth - KINDA_SMALL_NUMBER; }
	bool IsDoorCompliant(double ClearWidth, double Threshold) const { return ClearWidth >= DoorClearWidth - KINDA_SMALL_NUMBER && Threshold <= DoorThresholdMax + KINDA_SMALL_NUMBER; }
};
