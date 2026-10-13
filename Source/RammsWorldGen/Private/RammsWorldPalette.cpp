// Copyright ATDev. All Rights Reserved.
#include "RammsWorldPalette.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

bool URammsWorldPalette::HasSlot(FName Slot) const
{
	const FRammsPaletteSlot* S = Slots.Find(Slot);
	return S && (S->Meshes.Num() > 0 || !S->Material.IsNull() || !S->ActorClass.IsNull());
}

bool URammsWorldPalette::PickMesh(FName Slot, const FRandomStream& Rng, FRammsPaletteMesh& OutMesh) const
{
	const FRammsPaletteSlot* S = Slots.Find(Slot);
	if (!S || S->Meshes.Num() == 0)
	{
		return false;
	}
	float Total = 0.f;
	for (const FRammsPaletteMesh& M : S->Meshes) { Total += FMath::Max(0.f, M.Weight); }
	if (Total <= 0.f)
	{
		OutMesh = S->Meshes[0];
		return !OutMesh.Mesh.IsNull();
	}
	float Pick = Rng.FRand() * Total;
	for (const FRammsPaletteMesh& M : S->Meshes)
	{
		Pick -= FMath::Max(0.f, M.Weight);
		if (Pick <= 0.f)
		{
			OutMesh = M;
			return !OutMesh.Mesh.IsNull();
		}
	}
	OutMesh = S->Meshes.Last();
	return !OutMesh.Mesh.IsNull();
}

FString URammsWorldPalette::MaterialPath(FName Slot) const
{
	if (const FRammsPaletteSlot* S = Slots.Find(Slot))
	{
		if (!S->Material.IsNull())
		{
			return S->Material.ToSoftObjectPath().ToString();
		}
	}
	return DefaultMaterial.IsNull() ? FString() : DefaultMaterial.ToSoftObjectPath().ToString();
}

FString URammsWorldPalette::ActorClassPath(FName Slot) const
{
	if (const FRammsPaletteSlot* S = Slots.Find(Slot))
	{
		if (!S->ActorClass.IsNull())
		{
			return S->ActorClass.ToSoftObjectPath().ToString();
		}
	}
	return FString();
}

void URammsWorldPalette::SetSlot(FName Slot, const FRammsPaletteSlot& Value)
{
	Slots.FindOrAdd(Slot) = Value;
}

void URammsWorldPalette::AddMesh(FName Slot, const FRammsPaletteMesh& Mesh)
{
	Slots.FindOrAdd(Slot).Meshes.Add(Mesh);
}

void URammsWorldPalette::SetMaterial(FName Slot, TSoftObjectPtr<UMaterialInterface> Material)
{
	Slots.FindOrAdd(Slot).Material = Material;
}

void URammsWorldPalette::SetActorClass(FName Slot, TSoftClassPtr<AActor> ActorClass)
{
	Slots.FindOrAdd(Slot).ActorClass = ActorClass;
}

TArray<FName> URammsWorldPalette::MissingWellKnownSlots() const
{
	using namespace RammsPaletteSlots;
	static const FName WellKnown[] = {
		MatAsphalt, MatSidewalk, MatConcrete, MatCobble, MatFacade, MatFacadeAlt, MatGlass, MatPaint, MatWarning,
		MatGrass, MatLawn, MatPath, MatFoliage, MatTrunk, MatMetal, MatFloorWood, MatFloorTile, MatFloorCarpet,
		MatWallInterior, MatCeiling, MatDoor,
		Curb, SidewalkCorner, Bench, Bollard, BusStop, StreetLight, Railing, TrashBin, InfoBoard, TreePit, Tree, Shrub, Stairs,
		HazardAFrame, HazardScooter, HazardBarrier, HazardUpheaval, HazardCafe, HazardBinOut,
		FurnitureTable, FurnitureChair, FurnitureSofa, FurnitureBed, FurnitureCupboard, FurnitureDesk, FurnitureBooth,
		FurniturePlant, FurnitureLamp, FurnitureArmchair, FurnitureOfficeChair, FurnitureCoffeeTable, FurnitureBedTable,
		FurnitureTV, FurnitureTVStand, FurnitureToilet, FurnitureBathtub, FurniturePicture, FurnitureClutter, RoofUnit,
		MatMarking, MatMarkingYellow, MatStorefront, MatTrim, MatRoof, DoorActor };
	TArray<FName> Missing;
	for (const FName& N : WellKnown)
	{
		if (!HasSlot(N)) { Missing.Add(N); }
	}
	return Missing;
}
