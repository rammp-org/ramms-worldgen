// Copyright ATDev. All Rights Reserved.
#include "RammsSegmentationLabels.h"

#include "Components/BoxComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "RammsProcPiece.h"
#include "RammsWorldGenerator.h"
#include "RammsWorldGenModule.h"
#include "RammsWorldPalette.h"

const FString URammsSegmentationLabels::TagPrefix(TEXT("seg:"));

namespace
{
	/** Slabs this far above the generator (cm) are roofs, not ground. Above a raised plaza or a few steps,
	 *  below a single storey. */
	constexpr double RoofHeight = 250.0;
}

namespace
{
	/** Package path of an object path string: "/Game/A/M.M" and "/Game/A/M" both give "/Game/A/M". */
	FString PackageOf(const FString& ObjectPath)
	{
		FString P = ObjectPath;
		P.TrimStartAndEndInline();
		int32 Quote = INDEX_NONE;
		if (P.FindChar(TEXT('\''), Quote))
		{
			// Class'/Game/A/M.M' form
			P = P.RightChop(Quote + 1).LeftChop(1);
		}
		int32 Dot = INDEX_NONE;
		if (P.FindLastChar(TEXT('.'), Dot))
		{
			P.LeftInline(Dot);
		}
		return P;
	}

	bool HasClassTag(const TArray<FName>& Tags)
	{
		for (const FName& T : Tags)
		{
			if (T.ToString().StartsWith(URammsSegmentationLabels::TagPrefix, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/** Tag one primitive unless it (or its actor) already carries a class. */
	bool Tag(UPrimitiveComponent* C, FName Class)
	{
		if (!C || Class.IsNone() || HasClassTag(C->ComponentTags) || (C->GetOwner() && HasClassTag(C->GetOwner()->Tags)))
		{
			return false;
		}
		C->ComponentTags.Add(FName(*(URammsSegmentationLabels::TagPrefix + Class.ToString())));
		return true;
	}

	/** Reverse lookups from a palette: which slot a mesh, an actor class or a material came from. */
	struct FPaletteIndex
	{
		TMap<FString, FName> MeshSlot;	   // mesh package -> slot
		TMap<FString, FName> ActorSlot;	   // actor class path -> slot
		TMap<FString, FName> MaterialSlot; // material package -> Mat.* / Facade.Mat.* slot

		explicit FPaletteIndex(const URammsWorldPalette* Palette)
		{
			if (!Palette)
			{
				return;
			}
			TArray<FName> Names;
			Palette->Slots.GetKeys(Names);
			Names.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
			// A mesh can answer more than one slot (a chair placed as furniture and as a cafe hazard). What it IS
			// does not change with where it was put, so non-hazard slots claim it first.
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				for (const FName& Name : Names)
				{
					const FString S = Name.ToString();
					const bool bHazard = S.StartsWith(TEXT("Hazard."));
					if ((Pass == 0) == bHazard)
					{
						continue;
					}
					const FRammsPaletteSlot& Slot = Palette->Slots[Name];
					const bool bMat = S.StartsWith(TEXT("Mat.")) || S.StartsWith(RammsPaletteSlots::FacadeMatPrefix);
					if (bMat)
					{
						if (!Slot.Material.IsNull())
						{
							MaterialSlot.FindOrAdd(PackageOf(Slot.Material.ToString()), Name);
						}
						for (const TSoftObjectPtr<UMaterialInterface>& M : Slot.Materials)
						{
							if (!M.IsNull())
							{
								MaterialSlot.FindOrAdd(PackageOf(M.ToString()), Name);
							}
						}
						continue;
					}
					for (const FRammsPaletteMesh& M : Slot.Meshes)
					{
						if (!M.Mesh.IsNull())
						{
							MeshSlot.FindOrAdd(PackageOf(M.Mesh.ToString()), Name);
						}
						for (const FRammsPalettePart& Part : M.Parts)
						{
							if (!Part.Mesh.IsNull())
							{
								MeshSlot.FindOrAdd(PackageOf(Part.Mesh.ToString()), Name);
							}
						}
					}
					if (!Slot.ActorClass.IsNull())
					{
						ActorSlot.FindOrAdd(PackageOf(Slot.ActorClass.ToString()), Name);
					}
				}
			}
		}
	};

	const TCHAR* const PieceClasses[] = {
		TEXT("Road"), TEXT("Sidewalk"), TEXT("Concrete"), TEXT("Cobblestone"), TEXT("Wall.Exterior"), TEXT("Glass"),
		TEXT("RoadMarking"), TEXT("DetectableWarning"), TEXT("Lawn"), TEXT("Path"), TEXT("Foliage"), TEXT("Trunk"),
		TEXT("Metal"), TEXT("Floor"), TEXT("Wall.Interior"), TEXT("Ceiling"), TEXT("Door"), TEXT("Water"), TEXT("Gravel"),
		TEXT("Mud"), TEXT("Dirt"), TEXT("Partition"), TEXT("Storefront"), TEXT("Trim"), TEXT("Roof"), TEXT("Curb"),
		TEXT("CurbRamp"), TEXT("Stairs"), TEXT("Ramp"), TEXT("Bollard"), TEXT("Tree"), TEXT("Shrub"), TEXT("Planter"),
		TEXT("Column"), TEXT("Mound"), TEXT("Slab"), TEXT("Patch"), TEXT("Wall"), TEXT("Driveway"), TEXT("Threshold"),
		TEXT("Crosswalk"),
	};
} // namespace

FName URammsSegmentationLabels::ClassForSlot(FName Slot)
{
	const FString S = Slot.ToString();
	if (Slot == RammsPaletteSlots::DoorActor)
	{
		return FName(TEXT("Door"));
	}
	// "Facade.<Style>.<Part>": the style is a look, not a class.
	if (S.StartsWith(RammsPaletteSlots::FacadePrefix) && !S.StartsWith(RammsPaletteSlots::FacadeMatPrefix))
	{
		TArray<FString> Parts;
		S.ParseIntoArray(Parts, TEXT("."));
		if (Parts.Num() >= 3)
		{
			return FName(*(TEXT("Facade.") + Parts.Last()));
		}
	}
	return Slot;
}

FName URammsSegmentationLabels::ClassForMaterialSlot(FName MaterialSlot)
{
	const FString S = MaterialSlot.ToString();
	if (S.StartsWith(RammsPaletteSlots::FacadeMatPrefix))
	{
		return FName(TEXT("Wall.Exterior"));
	}
	struct FRule
	{
		const TCHAR* Prefix;
		const TCHAR* Class;
	};
	// Longest prefixes first where they overlap (Mat.Wall.* before Mat.Wall).
	static const FRule Rules[] = {
		{ TEXT("Mat.Asphalt"), TEXT("Road") },
		{ TEXT("Mat.Sidewalk"), TEXT("Sidewalk") },
		{ TEXT("Mat.Concrete"), TEXT("Concrete") },
		{ TEXT("Mat.Cobble"), TEXT("Cobblestone") },
		{ TEXT("Mat.FacadeAlt"), TEXT("Wall.Exterior") },
		{ TEXT("Mat.Facade"), TEXT("Wall.Exterior") },
		{ TEXT("Mat.Glass"), TEXT("Glass") },
		{ TEXT("Mat.Paint"), TEXT("RoadMarking") },
		{ TEXT("Mat.Marking"), TEXT("RoadMarking") },
		{ TEXT("Mat.Warning"), TEXT("DetectableWarning") },
		{ TEXT("Mat.Grass"), TEXT("Lawn") },
		{ TEXT("Mat.Lawn"), TEXT("Lawn") },
		{ TEXT("Mat.Path"), TEXT("Path") },
		{ TEXT("Mat.Foliage"), TEXT("Foliage") },
		{ TEXT("Mat.Trunk"), TEXT("Trunk") },
		{ TEXT("Mat.Metal"), TEXT("Metal") },
		{ TEXT("Mat.Floor"), TEXT("Floor") },
		{ TEXT("Mat.Wall"), TEXT("Wall.Interior") },
		{ TEXT("Mat.Ceiling"), TEXT("Ceiling") },
		{ TEXT("Mat.Door"), TEXT("Door") },
		{ TEXT("Mat.Water"), TEXT("Water") },
		{ TEXT("Mat.Gravel"), TEXT("Gravel") },
		{ TEXT("Mat.Mud"), TEXT("Mud") },
		{ TEXT("Mat.Dirt"), TEXT("Dirt") },
		{ TEXT("Mat.Partition"), TEXT("Partition") },
		{ TEXT("Mat.Storefront"), TEXT("Storefront") },
		{ TEXT("Mat.Trim"), TEXT("Trim") },
		{ TEXT("Mat.Roof"), TEXT("Roof") },
		{ TEXT("Mat.Curb"), TEXT("Curb") },
	};
	for (const FRule& R : Rules)
	{
		if (S.StartsWith(R.Prefix))
		{
			return FName(R.Class);
		}
	}
	return NAME_None;
}

FName URammsSegmentationLabels::ClassForPiece(const ARammsProcPiece* Piece, FName MaterialSlot)
{
	if (!Piece)
	{
		return NAME_None;
	}
	const FName Kind = Piece->ManifestKind;
	if (Kind == FName(TEXT("Hazard")))
	{
		return Piece->ManifestSubtype.IsNone() ? FName(TEXT("Hazard")) : FName(*(TEXT("Hazard.") + Piece->ManifestSubtype.ToString()));
	}
	if (Kind == FName(TEXT("Driveway")) || Kind == FName(TEXT("Threshold")) || Kind == FName(TEXT("Water")))
	{
		return Kind;
	}
	if (Kind == FName(TEXT("Crossing")))
	{
		return FName(TEXT("Crosswalk"));
	}
	const FName Surface = ClassForMaterialSlot(MaterialSlot);
	switch (Piece->GetEffectiveKind())
	{
		case ERammsProcPieceKind::Slab:
			return Surface.IsNone() ? FName(TEXT("Slab")) : Surface;
		case ERammsProcPieceKind::Patch:
			return Surface.IsNone() ? FName(TEXT("Patch")) : Surface;
		case ERammsProcPieceKind::Wall:
			return Surface.IsNone() ? FName(TEXT("Wall")) : Surface;
		case ERammsProcPieceKind::Curb:
			return FName(TEXT("Curb"));
		case ERammsProcPieceKind::CurbRamp:
			return FName(TEXT("CurbRamp"));
		case ERammsProcPieceKind::Step:
		case ERammsProcPieceKind::Stairs:
			return FName(TEXT("Stairs"));
		case ERammsProcPieceKind::Ramp:
			return FName(TEXT("Ramp"));
		case ERammsProcPieceKind::Door:
			return FName(TEXT("Door"));
		case ERammsProcPieceKind::Bollard:
			return FName(TEXT("Bollard"));
		case ERammsProcPieceKind::Tree:
			return FName(TEXT("Tree"));
		case ERammsProcPieceKind::Shrub:
			return FName(TEXT("Shrub"));
		case ERammsProcPieceKind::Planter:
			return FName(TEXT("Planter"));
		case ERammsProcPieceKind::Column:
			return FName(TEXT("Column"));
		case ERammsProcPieceKind::Roof:
			return FName(TEXT("Roof"));
		case ERammsProcPieceKind::Mound:
			return FName(TEXT("Mound"));
		case ERammsProcPieceKind::Decal:
		default:
			// A decal is drawn onto whatever it covers and takes that surface's class.
			return NAME_None;
	}
}

TArray<FName> URammsSegmentationLabels::ClassesForPalette(const URammsWorldPalette* Palette)
{
	TArray<FName> Out;
	if (Palette)
	{
		TArray<FName> Names;
		Palette->Slots.GetKeys(Names);
		for (const FName& N : Names)
		{
			const FString S = N.ToString();
			if (S.StartsWith(TEXT("Mat.")) || S.StartsWith(RammsPaletteSlots::FacadeMatPrefix) || S.StartsWith(TEXT("Decal.")))
			{
				continue;
			}
			Out.AddUnique(ClassForSlot(N));
		}
	}
	for (const TCHAR* C : PieceClasses)
	{
		Out.AddUnique(FName(C));
	}
	Out.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	return Out;
}

int32 URammsSegmentationLabels::LabelGeneratorOutput(ARammsWorldGenerator* Generator)
{
	UWorld* World = Generator ? Generator->GetWorld() : nullptr;
	if (!World)
	{
		return 0;
	}
	const FPaletteIndex Index(Generator->Params.Palette.LoadSynchronous());

	// What this generator made: its own components (PCG puts instanced meshes there), actors attached to it,
	// its procedural pieces, and partition actors inside its site.
	TSet<AActor*> Actors;
	Actors.Add(Generator);
	const FName GenTag(*FString::Printf(TEXT("RammsGen:%s"), *Generator->GetName()));
	const FBox	Site = Generator->SiteBounds ? Generator->SiteBounds->Bounds.GetBox().ExpandBy(500.0) : FBox(ForceInit);
	// With one generator in the world every procedural piece is its, including backdrop pieces (the hills
	// round a park) placed well outside the site. With several, the site decides.
	int32 Generators = 0;
	for (TActorIterator<ARammsWorldGenerator> It(World); It; ++It)
	{
		++Generators;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A == Generator)
		{
			continue;
		}
		const AActor* Parent = A->GetAttachParentActor();
		const AActor* Owner = A->GetOwner();
		const bool	  bOtherGen = (Parent && Parent != Generator && Parent->IsA<ARammsWorldGenerator>())
			|| (Owner && Owner != Generator && Owner->IsA<ARammsWorldGenerator>());
		if (bOtherGen)
		{
			continue;
		}
		bool bOurs = A->ActorHasTag(GenTag) || Parent == Generator || Owner == Generator || A->IsAttachedTo(Generator);
		if (!bOurs && (Generators == 1 || (Site.IsValid && Site.IsInside(A->GetActorLocation()))))
		{
			bOurs = A->IsA<ARammsProcPiece>() || A->GetClass()->GetName().Contains(TEXT("PCGPartitionActor"));
		}
		if (bOurs)
		{
			Actors.Add(A);
		}
	}

	int32 N = 0;
	for (AActor* A : Actors)
	{
		TInlineComponentArray<UPrimitiveComponent*> Prims(A);
		if (const ARammsProcPiece* Piece = Cast<ARammsProcPiece>(A))
		{
			const FName* MatSlot = Piece->MaterialPath.IsEmpty() ? nullptr : Index.MaterialSlot.Find(PackageOf(Piece->MaterialPath));
			FName		 Class = ClassForPiece(Piece, MatSlot ? *MatSlot : NAME_None);
			// A flat slab well above the ground is a roof, whatever surface it borrows its look from (flat
			// roofs are asphalt-, gravel- or concrete-surfaced slabs).
			if (Piece->GetEffectiveKind() == ERammsProcPieceKind::Slab && Piece->ManifestKind.IsNone()
				&& Piece->GetActorLocation().Z - Generator->GetActorLocation().Z > RoofHeight)
			{
				Class = FName(TEXT("Roof"));
			}
			for (UPrimitiveComponent* C : Prims)
			{
				N += Tag(C, Class) ? 1 : 0;
			}
			continue;
		}
		const FName* ActorSlot = A == Generator ? nullptr : Index.ActorSlot.Find(PackageOf(A->GetClass()->GetPathName()));
		for (UPrimitiveComponent* C : Prims)
		{
			FName Class;
			if (const UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(C))
			{
				if (const UStaticMesh* Mesh = SMC->GetStaticMesh())
				{
					if (const FName* Slot = Index.MeshSlot.Find(PackageOf(Mesh->GetPathName())))
					{
						Class = ClassForSlot(*Slot);
					}
				}
			}
			// Meshes standing in for a surface (the road and sidewalk are instanced boxes with a Mat.* override)
			// are what their material says.
			if (Class.IsNone())
			{
				if (const UMaterialInterface* M = C->GetMaterial(0))
				{
					if (const FName* MatSlot = Index.MaterialSlot.Find(PackageOf(M->GetPathName())))
					{
						Class = ClassForMaterialSlot(*MatSlot);
					}
				}
			}
			if (Class.IsNone() && ActorSlot)
			{
				Class = ClassForSlot(*ActorSlot);
			}
			N += Tag(C, Class) ? 1 : 0;
		}
	}
	UE_LOG(LogRammsWorldGen, Log, TEXT("%s: %d primitives labelled for segmentation"), *Generator->GetName(), N);
	return N;
}
