// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RammsWorldGenTypes.h"
#include "RammsSegmentationLabels.generated.h"

class ARammsProcPiece;
class ARammsWorldGenerator;
class URammsWorldPalette;

/**
 * Segmentation labels for generated worlds.
 *
 * Tags what a generator produced with its fine segmentation class, "seg:<Class>" on each primitive, which
 * CameraCapture's segmentation pass turns into custom-depth stencil values (see
 * UCameraCaptureSegmentationSubsystem; this module only writes tags and does not depend on it).
 *
 *  - Palette meshes (instanced or not) get their palette slot: "Bench", "Hazard.AFrame", "Furniture.Chair".
 *    Facade modules drop their style: "Facade.<Style>.Win" is "Facade.Win". Composite entries (houses) label
 *    every part with the entry's slot.
 *  - Procedural pieces get a class from what they are: a slab or patch by its surface ("Road", "Sidewalk",
 *    "Lawn", "Floor", "Water"...), a wall by its material ("Wall.Exterior", "Wall.Interior", "Partition"),
 *    other kinds by kind ("Curb", "CurbRamp", "Stairs", "Ramp", "Door", "Tree"...), and hazards as
 *    "Hazard.<Subtype>".
 *
 * Existing class tags are kept, so a hand-placed label wins. The coarse classes these roll up to are the
 * project's config (DefaultGame.ini, CameraCaptureSegmentationSettings).
 */
UCLASS()
class RAMMSWORLDGEN_API URammsSegmentationLabels : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** The tag prefix CameraCapture reads by default. */
	static const FString TagPrefix;

	/** Label everything a generator produced. Returns how many primitives were given a class. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen|Segmentation")
	static int32 LabelGeneratorOutput(ARammsWorldGenerator* Generator);

	/** The fine class of a palette slot ("Facade.Classic.Win" -> "Facade.Win", "DoorActor" -> "Door"). */
	UFUNCTION(BlueprintPure, Category = "Ramms|WorldGen|Segmentation")
	static FName ClassForSlot(FName Slot);

	/** The fine class of a surface material slot ("Mat.Asphalt" -> "Road"); None for one that names no surface. */
	UFUNCTION(BlueprintPure, Category = "Ramms|WorldGen|Segmentation")
	static FName ClassForMaterialSlot(FName MaterialSlot);

	/** The fine class of a procedural piece, given the material slot its primary material came from (None if unknown). */
	static FName ClassForPiece(const ARammsProcPiece* Piece, FName MaterialSlot);

	/** Every fine class this module can produce for a palette (its slots, plus the piece classes), for building
	 *  a class table. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen|Segmentation")
	static TArray<FName> ClassesForPalette(const URammsWorldPalette* Palette);
};
