// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RammsWorldGenTypes.h"
#include "RammsWorldPreset.generated.h"

class URammsWorldPalette;
class URammsAccessibilityProfile;
class UPCGGraphInterface;

/**
 * Everything needed to generate one family of worlds: the scene kind, the
 * parameter block, which palette answers the slots, which accessibility
 * profile sets the thresholds, and which PCG graph does the work.
 *
 * A level places an ARammsWorldGenerator, points it at a preset, and picks a
 * seed; different seeds give different worlds of the same family.
 */
UCLASS(BlueprintType)
class RAMMSWORLDGEN_API URammsWorldPreset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset")
	ERammsSceneKind SceneKind = ERammsSceneKind::Downtown;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset", meta = (MultiLine = true))
	FText Description;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset")
	FRammsWorldGenParams Params;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset")
	TSoftObjectPtr<URammsWorldPalette> Palette;

	/** Optional; when set, its thresholds overwrite the matching Params before generation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset")
	TSoftObjectPtr<URammsAccessibilityProfile> Profile;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preset")
	TSoftObjectPtr<UPCGGraphInterface> Graph;

	/** Params with the profile applied, ready for a generator. */
	UFUNCTION(BlueprintPure, Category = "Ramms|WorldGen")
	FRammsWorldGenParams ResolveParams() const;
};
