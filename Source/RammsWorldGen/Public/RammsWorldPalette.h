// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RammsWorldPalette.generated.h"

class UStaticMesh;
class UMaterialInterface;

/** One mesh a palette slot can pick. Footprint and Height are read off the
 *  mesh bounds when the palette is built so placement can keep clearances
 *  without loading the mesh at generation time. */
USTRUCT(BlueprintType)
struct RAMMSWORLDGEN_API FRammsPaletteMesh
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette", meta = (ClampMin = 0))
	float Weight = 1.0f;

	/** Plan extent (X along the mesh's forward, Y across), cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	FVector2D Footprint = FVector2D(100.0, 100.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	double Height = 100.0;

	/** Applied before the point transform: lets a mesh whose pivot is not at its base/centre still sit on the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	FVector LocationOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	FRotator RotationOffset = FRotator::ZeroRotator;

	/** Uniform scale applied on top of any per-point scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette", meta = (ClampMin = 0.01))
	float Scale = 1.0f;
};

/** A semantic slot: what the generator asks for ("Bench", "Mat.Sidewalk",
 *  "Hazard.AFrame") rather than which asset answers. */
USTRUCT(BlueprintType)
struct RAMMSWORLDGEN_API FRammsPaletteSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	TArray<FRammsPaletteMesh> Meshes;

	/** For Mat.* slots, and as a material override for procedural pieces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	TSoftObjectPtr<UMaterialInterface> Material;

	/** For slots answered by an actor class rather than a mesh (doors, buttons). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	TSoftClassPtr<AActor> ActorClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	FText Note;
};

/** Well-known slot names. Palettes may add their own; nodes only ask for these. */
namespace RammsPaletteSlots
{
	// materials
	inline const FName MatAsphalt(TEXT("Mat.Asphalt"));
	inline const FName MatSidewalk(TEXT("Mat.Sidewalk"));
	inline const FName MatConcrete(TEXT("Mat.Concrete"));
	inline const FName MatCobble(TEXT("Mat.Cobble"));
	inline const FName MatFacade(TEXT("Mat.Facade"));
	inline const FName MatFacadeAlt(TEXT("Mat.FacadeAlt"));
	inline const FName MatGlass(TEXT("Mat.Glass"));
	inline const FName MatPaint(TEXT("Mat.Paint"));
	inline const FName MatWarning(TEXT("Mat.Warning"));
	inline const FName MatGrass(TEXT("Mat.Grass"));
	inline const FName MatLawn(TEXT("Mat.Lawn"));
	inline const FName MatPath(TEXT("Mat.Path"));
	inline const FName MatFoliage(TEXT("Mat.Foliage"));
	inline const FName MatTrunk(TEXT("Mat.Trunk"));
	inline const FName MatMetal(TEXT("Mat.Metal"));
	inline const FName MatFloorWood(TEXT("Mat.Floor.Wood"));
	inline const FName MatFloorTile(TEXT("Mat.Floor.Tile"));
	inline const FName MatFloorCarpet(TEXT("Mat.Floor.Carpet"));
	inline const FName MatWallInterior(TEXT("Mat.Wall.Interior"));
	inline const FName MatCeiling(TEXT("Mat.Ceiling"));
	inline const FName MatDoor(TEXT("Mat.Door"));
	// street meshes
	inline const FName Curb(TEXT("Curb"));
	inline const FName SidewalkCorner(TEXT("SidewalkCorner"));
	inline const FName Bench(TEXT("Bench"));
	inline const FName Bollard(TEXT("Bollard"));
	inline const FName BusStop(TEXT("BusStop"));
	inline const FName StreetLight(TEXT("StreetLight"));
	inline const FName Railing(TEXT("Railing"));
	inline const FName TrashBin(TEXT("TrashBin"));
	inline const FName InfoBoard(TEXT("InfoBoard"));
	inline const FName TreePit(TEXT("TreePit"));
	inline const FName Tree(TEXT("Tree"));
	inline const FName Shrub(TEXT("Shrub"));
	inline const FName Stairs(TEXT("Stairs"));
	// hazards (meshes that intrude into the access route)
	inline const FName HazardAFrame(TEXT("Hazard.AFrame"));
	inline const FName HazardScooter(TEXT("Hazard.Scooter"));
	inline const FName HazardBarrier(TEXT("Hazard.Barrier"));
	inline const FName HazardUpheaval(TEXT("Hazard.Upheaval"));
	inline const FName HazardCafe(TEXT("Hazard.Cafe"));
	inline const FName HazardBinOut(TEXT("Hazard.BinOut"));
	// indoor
	inline const FName FurnitureTable(TEXT("Furniture.Table"));
	inline const FName FurnitureChair(TEXT("Furniture.Chair"));
	inline const FName FurnitureSofa(TEXT("Furniture.Sofa"));
	inline const FName FurnitureBed(TEXT("Furniture.Bed"));
	inline const FName FurnitureCupboard(TEXT("Furniture.Cupboard"));
	inline const FName FurnitureDesk(TEXT("Furniture.Desk"));
	inline const FName FurnitureBooth(TEXT("Furniture.Booth"));
	inline const FName FurniturePlant(TEXT("Furniture.Plant"));
	inline const FName FurnitureLamp(TEXT("Furniture.Lamp"));
	inline const FName FurnitureArmchair(TEXT("Furniture.Armchair"));
	inline const FName FurnitureOfficeChair(TEXT("Furniture.OfficeChair"));
	inline const FName FurnitureCoffeeTable(TEXT("Furniture.CoffeeTable"));
	inline const FName FurnitureBedTable(TEXT("Furniture.BedTable"));
	inline const FName FurnitureTV(TEXT("Furniture.TV"));
	inline const FName FurnitureTVStand(TEXT("Furniture.TVStand"));
	inline const FName FurnitureToilet(TEXT("Furniture.Toilet"));
	inline const FName FurnitureBathtub(TEXT("Furniture.Bathtub"));
	inline const FName FurniturePicture(TEXT("Furniture.Picture"));
	inline const FName FurnitureClutter(TEXT("Furniture.Clutter"));
	// building dressing
	inline const FName RoofUnit(TEXT("RoofUnit"));
	inline const FName MatMarking(TEXT("Mat.Marking"));
	inline const FName MatMarkingYellow(TEXT("Mat.MarkingYellow"));
	inline const FName MatStorefront(TEXT("Mat.Storefront"));
	inline const FName MatTrim(TEXT("Mat.Trim"));
	inline const FName MatRoof(TEXT("Mat.Roof"));
	inline const FName DoorActor(TEXT("DoorActor"));
}

/**
 * Maps the generator's semantic slots to concrete assets. Swap the palette to
 * re-skin a world with a different Fab pack without touching a graph.
 */
UCLASS(BlueprintType)
class RAMMSWORLDGEN_API URammsWorldPalette : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	TMap<FName, FRammsPaletteSlot> Slots;

	/** Fallback material for any Mat.* slot the palette does not fill. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Palette")
	TSoftObjectPtr<UMaterialInterface> DefaultMaterial;

	UFUNCTION(BlueprintPure, Category = "Ramms|WorldGen")
	bool HasSlot(FName Slot) const;

	/** Weighted pick. Returns false when the slot is empty or missing. */
	bool PickMesh(FName Slot, const FRandomStream& Rng, FRammsPaletteMesh& OutMesh) const;

	/** Soft object path string of the slot's material, or DefaultMaterial, or empty. */
	UFUNCTION(BlueprintPure, Category = "Ramms|WorldGen")
	FString MaterialPath(FName Slot) const;

	UFUNCTION(BlueprintPure, Category = "Ramms|WorldGen")
	FString ActorClassPath(FName Slot) const;

	/** Replace or create a slot. Used by the Python palette builder. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	void SetSlot(FName Slot, const FRammsPaletteSlot& Value);

	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	void AddMesh(FName Slot, const FRammsPaletteMesh& Mesh);

	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	void SetMaterial(FName Slot, TSoftObjectPtr<UMaterialInterface> Material);

	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	void SetActorClass(FName Slot, TSoftClassPtr<AActor> ActorClass);

	/** Slot names that nodes may ask for but this palette leaves empty. */
	UFUNCTION(BlueprintPure, Category = "Ramms|WorldGen")
	TArray<FName> MissingWellKnownSlots() const;
};
