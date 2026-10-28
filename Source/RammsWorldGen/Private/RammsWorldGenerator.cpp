// Copyright ATDev. All Rights Reserved.
#include "RammsWorldGenerator.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Components/SkeletalMeshComponent.h"
#include "TimerManager.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGGraph.h"
#include "PCGParamData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/PCGMetadataAttributeTraits.h"
#include "RammsAccessibilityProfile.h"
#include "RammsProcPiece.h"
#include "RammsSegmentationLabels.h"
#include "RammsWorldGenGraphLibrary.h"
#include "RammsWorldGenModule.h"
#include "RammsWorldPalette.h"
#include "RammsWorldPreset.h"
#include "UObject/UnrealType.h"

ARammsWorldGenerator::ARammsWorldGenerator()
{
	PrimaryActorTick.bCanEverTick = false;

	SiteBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("SiteBounds"));
	SetRootComponent(SiteBounds);
	SiteBounds->SetBoxExtent(FVector(6000.0, 6000.0, 500.0));
	SiteBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SiteBounds->SetGenerateOverlapEvents(false);
	SiteBounds->SetLineThickness(4.0f);
	SiteBounds->ShapeColor = FColor(60, 200, 120);

	PCGComponent = CreateDefaultSubobject<UPCGComponent>(TEXT("PCG"));
	PCGComponent->GenerationTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
	PCGComponent->bActivated = true;
#if WITH_EDITORONLY_DATA
	// The graph spawns actors inside the component's own bounds; with editor
	// auto-refresh on, every generation would re-trigger itself several times
	// over and the outputs pile up. Generation is explicit (Regenerate()).
	PCGComponent->bRegenerateInEditor = false;
#endif
}

void ARammsWorldGenerator::EnsureBound()
{
	// Editor-spawned actors do not reliably get PostInitializeComponents, so bind lazily from every entry point.
	if (PCGComponent && !bBoundDelegate)
	{
		PCGComponent->OnPCGGraphGeneratedDelegate.AddUObject(this, &ARammsWorldGenerator::HandleGenerated);
		PCGComponent->OnPCGGraphCancelledDelegate.AddUObject(this, &ARammsWorldGenerator::HandleCancelled);
		bBoundDelegate = true;
	}
}

void ARammsWorldGenerator::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	EnsureBound();
}

void ARammsWorldGenerator::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	EnsureBound();
	ResolveParams();
}

void ARammsWorldGenerator::BeginPlay()
{
	Super::BeginPlay();
	if (bRandomizeSeedOnBeginPlay)
	{
		Seed = FMath::Rand();
	}
	if (bGenerateOnBeginPlay)
	{
		Regenerate();
	}
	else
	{
		// A world generated before segmentation labels existed carries none; label what was saved. Cheap (one
		// pass over the site's actors) and a no-op for anything already labelled.
		URammsSegmentationLabels::LabelGeneratorOutput(this);
	}
}

void ARammsWorldGenerator::EndPlay(const EEndPlayReason::Type Reason)
{
	// Never leave a pawn frozen because the generator went away mid-generation.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReleaseTimer);
		World->GetTimerManager().ClearTimer(HoldSafetyTimer);
	}
	ReleasePlayers();
	Super::EndPlay(Reason);
}

// --- params ------------------------------------------------------------------------

void ARammsWorldGenerator::ResolveParams()
{
	if (Preset)
	{
		Params = Preset->ResolveParams();
	}

	// Named overrides, by reflection so the key list is the only thing to maintain.
	if (OverrideKeys.Num() > 0)
	{
		const UScriptStruct* S = FRammsWorldGenParams::StaticStruct();
		for (TFieldIterator<FProperty> It(S); It; ++It)
		{
			if (OverrideKeys.Contains(It->GetFName()))
			{
				It->CopyCompleteValue_InContainer(&Params, &ParamOverrides);
			}
		}
	}
	// After the overrides, so an override key with no palette set still falls back to the preset's.
	if (Params.Palette.IsNull() && Preset)
	{
		Params.Palette = Preset->Palette;
	}

	if (SiteBounds)
	{
		SiteBounds->SetBoxExtent(FVector(FMath::Max(Params.SiteSizeX, 100.0) * 0.5, FMath::Max(Params.SiteSizeY, 100.0) * 0.5, 500.0));
	}
	if (PCGComponent)
	{
		PCGComponent->Seed = Seed;
	}
}

bool ARammsWorldGenerator::SetParam(FName Name, double Value)
{
	const UScriptStruct* S = FRammsWorldGenParams::StaticStruct();
	FProperty* P = S->FindPropertyByName(Name);
	if (!P)
	{
		return false;
	}
	void* Ptr = P->ContainerPtrToValuePtr<void>(&ParamOverrides);
	if (FDoubleProperty* D = CastField<FDoubleProperty>(P)) { D->SetPropertyValue(Ptr, Value); }
	else if (FFloatProperty* F = CastField<FFloatProperty>(P)) { F->SetPropertyValue(Ptr, static_cast<float>(Value)); }
	else if (FIntProperty* I = CastField<FIntProperty>(P)) { I->SetPropertyValue(Ptr, FMath::RoundToInt(Value)); }
	else if (FBoolProperty* B = CastField<FBoolProperty>(P)) { B->SetPropertyValue(Ptr, Value != 0.0); }
	else { return false; }
	OverrideKeys.Add(Name);
	ResolveParams();
	return true;
}

bool ARammsWorldGenerator::GetParam(FName Name, double& OutValue) const
{
	const UScriptStruct* S = FRammsWorldGenParams::StaticStruct();
	const FProperty* P = S->FindPropertyByName(Name);
	if (!P)
	{
		return false;
	}
	const void* Ptr = P->ContainerPtrToValuePtr<void>(&Params);
	if (const FDoubleProperty* D = CastField<FDoubleProperty>(P)) { OutValue = D->GetPropertyValue(Ptr); return true; }
	if (const FFloatProperty* F = CastField<FFloatProperty>(P)) { OutValue = F->GetPropertyValue(Ptr); return true; }
	if (const FIntProperty* I = CastField<FIntProperty>(P)) { OutValue = I->GetPropertyValue(Ptr); return true; }
	if (const FBoolProperty* B = CastField<FBoolProperty>(P)) { OutValue = B->GetPropertyValue(Ptr) ? 1.0 : 0.0; return true; }
	return false;
}

TArray<FName> ARammsWorldGenerator::GetParamNames() const
{
	TArray<FName> Names;
	for (TFieldIterator<FProperty> It(FRammsWorldGenParams::StaticStruct()); It; ++It)
	{
		if (CastField<FDoubleProperty>(*It) || CastField<FFloatProperty>(*It) || CastField<FIntProperty>(*It) || CastField<FBoolProperty>(*It))
		{
			Names.Add(It->GetFName());
		}
	}
	return Names;
}

// --- generation ----------------------------------------------------------------------

bool ARammsWorldGenerator::SetPalette(const FString& Palette)
{
	const FName Key(TEXT("Palette"));
	if (Palette.IsEmpty())
	{
		ParamOverrides.Palette.Reset();
		OverrideKeys.Remove(Key);
		ResolveParams();
		return true;
	}
	FString Path = Palette;
	if (!Path.StartsWith(TEXT("/")))
	{
		Path = FString::Printf(TEXT("/RammsWorldGen/Palettes/%s"), *Path);
	}
	if (!Path.Contains(TEXT(".")))
	{
		Path = FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path));
	}
	URammsWorldPalette* Loaded = LoadObject<URammsWorldPalette>(nullptr, *Path);
	if (!Loaded)
	{
		UE_LOG(LogRammsWorldGen, Warning, TEXT("%s: palette '%s' not found"), *GetName(), *Palette);
		return false;
	}
	ParamOverrides.Palette = Loaded;
	OverrideKeys.Add(Key);
	ResolveParams();
	return true;
}

FString ARammsWorldGenerator::GetPalettePath() const
{
	return Params.Palette.ToSoftObjectPath().ToString();
}

bool ARammsWorldGenerator::Regenerate()
{
	if (!PCGComponent)
	{
		return false;
	}
	ResolveParams();

	UPCGGraphInterface* Graph = nullptr;
	if (Preset && !Preset->Graph.IsNull())
	{
		Graph = Preset->Graph.LoadSynchronous();
	}
	if (Graph && PCGComponent->GetGraph() != Graph)
	{
		PCGComponent->SetGraph(Graph);
	}
	if (!PCGComponent->GetGraph())
	{
		UE_LOG(LogRammsWorldGen, Warning, TEXT("%s: no PCG graph on the preset or the component; nothing to generate"), *GetName());
		return false;
	}

	EnsureBound();
	Manifest = FRammsWorldManifest();
	Manifest.Seed = Seed;
	Manifest.Preset = Preset ? Preset->GetFName() : NAME_None;
	Manifest.SceneKind = Preset ? Preset->SceneKind : ERammsSceneKind::Downtown;
	Manifest.Palette = Params.Palette.ToSoftObjectPath().ToString();
	bPlayersTaken = false;

	UE_LOG(LogRammsWorldGen, Log, TEXT("%s: generating preset=%s seed=%d site=%.0fx%.0f"), *GetName(), *Manifest.Preset.ToString(), Seed, Params.SiteSizeX, Params.SiteSizeY);
	HoldPlayers();
	PCGComponent->GenerateLocal(/*bForce=*/true);
	return true;
}

bool ARammsWorldGenerator::RegenerateWithSeed(int32 NewSeed)
{
	Seed = NewSeed;
	return Regenerate();
}

void ARammsWorldGenerator::Randomize()
{
	Seed = FMath::Rand();
	Regenerate();
}

void ARammsWorldGenerator::Cleanup()
{
	if (PCGComponent)
	{
		PCGComponent->CleanupLocal(/*bRemoveComponents=*/true);
	}
	Manifest = FRammsWorldManifest();
	ReleasePlayers(); // never leave a pawn frozen if a regeneration is abandoned
}

bool ARammsWorldGenerator::IsGenerating() const
{
	return PCGComponent && PCGComponent->IsGenerating();
}

void ARammsWorldGenerator::HandleCancelled(UPCGComponent* Component)
{
	UE_LOG(LogRammsWorldGen, Warning, TEXT("%s: generation cancelled"), *GetName());
	ReleasePlayers();
}

void ARammsWorldGenerator::HandleGenerated(UPCGComponent* Component)
{
	BuildManifestFromOutput();
	CollectProcPieces();
	Manifest.Bounds = SiteBounds ? SiteBounds->Bounds.GetBox() : FBox(ForceInit);
	ApplyGlassVariation();
	URammsSegmentationLabels::LabelGeneratorOutput(this);
	UE_LOG(LogRammsWorldGen, Log, TEXT("%s: generated seed=%d features=%d routes=%d issues=%d"), *GetName(), Seed, Manifest.Features.Num(), Manifest.Routes.Num(), Manifest.CountIssues());
	if (bPlacePlayersOnGenerate && !bPlayersTaken && GetWorld() && GetWorld()->IsGameWorld())
	{
		PlacePlayerStart();
		PlacePlayers();
	}
	OnWorldGenerated.Broadcast(Manifest);
}

// Each placed glass instance (bus-shelter panes, car windows, ...) gets its own frost / opacity / tint / bump
// as per-instance custom data, drawn from the seed and its position. PCG puts its instanced meshes on this
// actor (or, for a partitioned graph, on partition actors inside the site), so both are visited.
int32 ARammsWorldGenerator::ApplyGlassVariation()
{
	const float V = static_cast<float>(Params.GlassVariation);
	const float Fc = static_cast<float>(Params.GlassFrostChance), Tc = static_cast<float>(Params.GlassTintChance), Bc = static_cast<float>(Params.GlassBumpChance);
	int32 N = URammsWorldGenGraphLibrary::VaryGlassInstances(this, Seed, V, Fc, Tc, Bc);
	UWorld* World = GetWorld();
	if (World && SiteBounds)
	{
		const FBox Site = SiteBounds->Bounds.GetBox().ExpandBy(500.0);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (A != this && A->GetClass()->GetName().Contains(TEXT("PCGPartitionActor")) && Site.IsInside(A->GetActorLocation()))
			{
				N += URammsWorldGenGraphLibrary::VaryGlassInstances(A, Seed, V, Fc, Tc, Bc);
			}
		}
	}
	if (N > 0)
	{
		UE_LOG(LogRammsWorldGen, Log, TEXT("%s: glass variation %.2f on %d instances"), *GetName(), V, N);
	}
	return N;
}

// The manifest node emits one attribute set whose "Json" attribute is a serialized
// FRammsWorldManifest (features + routes + start). Merge it into ours.
void ARammsWorldGenerator::BuildManifestFromOutput()
{
	if (!PCGComponent)
	{
		return;
	}
	const FPCGDataCollection& Output = PCGComponent->GetGeneratedGraphOutput();
	for (const FPCGTaggedData& Tagged : Output.TaggedData)
	{
		const UPCGParamData* Param = Cast<UPCGParamData>(Tagged.Data);
		if (!Param || !Param->Metadata)
		{
			continue;
		}
		const FPCGMetadataAttributeBase* Base = Param->Metadata->GetConstAttribute(FName(TEXT("Json")));
		if (!Base || Base->GetTypeId() != PCG::Private::MetadataTypes<FString>::Id)
		{
			continue;
		}
		const FString Json = static_cast<const FPCGMetadataAttribute<FString>*>(Base)->GetValueFromItemKey(PCGFirstEntryKey);
		FRammsWorldManifest Partial;
		if (FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Partial, 0, 0))
		{
			Manifest.Features.Append(Partial.Features);
			Manifest.Routes.Append(Partial.Routes);
			if (!Partial.SuggestedStart.Equals(FTransform::Identity))
			{
				Manifest.SuggestedStart = Partial.SuggestedStart;
			}
		}
		else
		{
			UE_LOG(LogRammsWorldGen, Warning, TEXT("%s: manifest JSON from the graph did not parse"), *GetName());
		}
	}
}

// Procedural pieces spawned by the graph carry their own ground truth; gather the
// ones generated for this actor (Spawn Actor tags them with the generator's name).
void ARammsWorldGenerator::CollectProcPieces()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const FName Tag(*FString::Printf(TEXT("RammsGen:%s"), *GetName()));
	for (TActorIterator<ARammsProcPiece> It(World); It; ++It)
	{
		ARammsProcPiece* Piece = *It;
		// A piece attached to (or owned by) another generator is that generator's, wherever it stands.
		const AActor* Parent = Piece->GetAttachParentActor();
		const AActor* PieceOwner = Piece->GetOwner();
		const bool bOther = (Parent && Parent != this && Parent->IsA<ARammsWorldGenerator>()) || (PieceOwner && PieceOwner != this && PieceOwner->IsA<ARammsWorldGenerator>());
		const bool bOurs = !bOther && (Piece->ActorHasTag(Tag) || Parent == this || PieceOwner == this
			|| (Piece->ActorHasTag(FName(TEXT("RammsProcPiece"))) && SiteBounds && SiteBounds->Bounds.GetBox().ExpandBy(500.0).IsInside(Piece->GetActorLocation())));
		if (!bOurs || Piece->ManifestKind.IsNone())
		{
			continue;
		}
		Manifest.Features.Add(Piece->ToManifestFeature());
	}
}

// --- outputs ---------------------------------------------------------------------------

FString ARammsWorldGenerator::GetManifestJson() const
{
	FString Out;
	FJsonObjectConverter::UStructToJsonObjectString(Manifest, Out, 0, 0, 0, nullptr, /*bPrettyPrint=*/true);
	return Out;
}

FString ARammsWorldGenerator::ExportManifest(const FString& AbsolutePath)
{
	FString Path = AbsolutePath;
	if (Path.IsEmpty())
	{
		Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WorldGen"),
			FString::Printf(TEXT("%s_%s_%d.json"), *GetName(), *Manifest.Preset.ToString(), Seed));
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!FFileHelper::SaveStringToFile(GetManifestJson(), *Path))
	{
		UE_LOG(LogRammsWorldGen, Warning, TEXT("%s: could not write manifest to %s"), *GetName(), *Path);
		return FString();
	}
	return Path;
}

void ARammsWorldGenerator::PlacePlayerStart()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FTransform Start = Manifest.SuggestedStart;
	if (Start.Equals(FTransform::Identity))
	{
		// No route yet: stand at the site centre.
		Start = FTransform(GetActorLocation() + FVector(0.0, 0.0, 10.0));
	}
	APlayerStart* Existing = nullptr;
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		Existing = *It;
		break;
	}
	if (!Existing)
	{
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Existing = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), Start, SP);
#if WITH_EDITOR
		if (Existing)
		{
			Existing->SetActorLabel(TEXT("PlayerStart_WorldGen"));
		}
#endif
	}
	else
	{
		Existing->SetActorTransform(Start);
	}
}

// --- players (play worlds) -------------------------------------------------------------
//
// Rebuilding the world under a physics-simulated robot drops it through the old floor and lets the new
// geometry spawn into it, which tips it over. While a play world regenerates, the player pawns' simulated
// bodies are made kinematic; when the new world is in, the pawns are teleported to its suggested start and,
// a moment later, their physics is switched back on at rest. Pawns driven by an external simulator (e.g.
// MuJoCo) have no Unreal-simulated bodies and are only teleported; that backend has to reset its own state.

namespace
{
	bool IsAnySimulating(const UPrimitiveComponent* C)
	{
		if (const USkeletalMeshComponent* Sk = Cast<USkeletalMeshComponent>(C))
		{
			return Sk->IsAnySimulatingPhysics();
		}
		return C && C->IsSimulatingPhysics();
	}

	template <typename F>
	void ForEachPlayerPawn(UWorld* World, F&& Fn)
	{
		if (!World)
		{
			return;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (APlayerController* PC = It->Get())
			{
				if (APawn* Pawn = PC->GetPawn())
				{
					Fn(Pawn);
				}
			}
		}
	}
}

void ARammsWorldGenerator::HoldPlayers()
{
	UWorld* World = GetWorld();
	if (!bPlacePlayersOnGenerate || !World || !World->IsGameWorld())
	{
		return;
	}
	World->GetTimerManager().ClearTimer(ReleaseTimer);
	// Another generator still holding the players (two regenerating at once): this one, started last, takes the
	// held bodies over and places the players when it is done; the other leaves them alone.
	for (TActorIterator<ARammsWorldGenerator> It(World); It; ++It)
	{
		ARammsWorldGenerator* Other = *It;
		if (Other == this || Other->HeldBodies.Num() == 0)
		{
			continue;
		}
		World->GetTimerManager().ClearTimer(Other->ReleaseTimer);
		World->GetTimerManager().ClearTimer(Other->HoldSafetyTimer);
		HeldBodies.Append(Other->HeldBodies);
		Other->HeldBodies.Reset();
		Other->bPlayersTaken = true;
	}
	ForEachPlayerPawn(World, [this](APawn* Pawn)
	{
		TArray<UPrimitiveComponent*> Prims;
		Pawn->GetComponents(Prims);
		const FTransform ActorXf = Pawn->GetActorTransform();
		for (UPrimitiveComponent* C : Prims)
		{
			if (!IsAnySimulating(C) || HeldBodies.ContainsByPredicate([C](const FHeldBody& H) { return H.Component.Get() == C; }))
			{
				continue;
			}
			C->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
			C->SetAllPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
			FHeldBody H{ C, C->GetComponentTransform().GetRelativeTransform(ActorXf) };
			if (USkeletalMeshComponent* Sk = Cast<USkeletalMeshComponent>(C))
			{
				int32 Sim = 0;
				for (const FBodyInstance* B : Sk->Bodies)
				{
					if (B && B->IsInstanceSimulatingPhysics() && B->BodySetup.IsValid())
					{
						H.SimulatedBones.Add(B->BodySetup->BoneName);
						++Sim;
					}
				}
				H.bAllBodies = Sim == Sk->Bodies.Num();
			}
			HeldBodies.Add(H);
			C->SetSimulatePhysics(false);
		}
	});
	if (HeldBodies.Num() > 0)
	{
		// If the generation never reports back (cancelled, failed), let the players go anyway.
		World->GetTimerManager().SetTimer(HoldSafetyTimer, this, &ARammsWorldGenerator::ReleasePlayers, 120.0f, false);
		UE_LOG(LogRammsWorldGen, Log, TEXT("%s: holding %d simulated player bodies while the world is rebuilt"), *GetName(), HeldBodies.Num());
	}
}

void ARammsWorldGenerator::PlacePlayers()
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}
	FTransform Start = Manifest.SuggestedStart;
	if (Start.Equals(FTransform::Identity))
	{
		Start = FTransform(GetActorLocation() + FVector(0.0, 0.0, 10.0));
	}
	const FRotator Yaw(0.0, Start.Rotator().Yaw, 0.0);
	const FVector Loc = Start.GetLocation() + FVector(0.0, 0.0, PlayerPlacementLift);
	int32 Placed = 0;
	ForEachPlayerPawn(World, [&](APawn* Pawn)
	{
		Pawn->SetActorLocationAndRotation(Loc, Yaw, false, nullptr, ETeleportType::ResetPhysics);
		const FTransform ActorXf = Pawn->GetActorTransform();
		// Bodies that were simulating were detached from the actor; put them back where they belong on it.
		for (const FHeldBody& H : HeldBodies)
		{
			UPrimitiveComponent* C = H.Component.Get();
			if (C && C->GetOwner() == Pawn && C != Pawn->GetRootComponent())
			{
				C->SetWorldTransform(H.RelativeToActor * ActorXf, false, nullptr, ETeleportType::ResetPhysics);
			}
		}
		TArray<UPrimitiveComponent*> Prims;
		Pawn->GetComponents(Prims);
		for (UPrimitiveComponent* C : Prims)
		{
			C->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
			C->SetAllPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
		}
		++Placed;
	});
	if (Placed > 0)
	{
		UE_LOG(LogRammsWorldGen, Log, TEXT("%s: placed %d player pawn(s) at %s yaw %.0f"), *GetName(), Placed, *Loc.ToString(), Yaw.Yaw);
	}
	if (HeldBodies.Num() > 0)
	{
		World->GetTimerManager().SetTimer(ReleaseTimer, this, &ARammsWorldGenerator::ReleasePlayers, FMath::Max(PlayerReleaseDelay, 0.01f), false);
	}
}

void ARammsWorldGenerator::ReleasePlayers()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(HoldSafetyTimer);
	}
	for (const FHeldBody& H : HeldBodies)
	{
		if (UPrimitiveComponent* C = H.Component.Get())
		{
			USkeletalMeshComponent* Sk = Cast<USkeletalMeshComponent>(C);
			if (Sk && !H.bAllBodies)
			{
				// Only the bodies that were simulating before.
				for (const FName& Bone : H.SimulatedBones)
				{
					if (FBodyInstance* B = Sk->GetBodyInstance(Bone))
					{
						B->SetInstanceSimulatePhysics(true);
					}
				}
			}
			else
			{
				C->SetSimulatePhysics(true);
			}
			C->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
			C->SetAllPhysicsAngularVelocityInDegrees(FVector::ZeroVector);
			C->WakeAllRigidBodies();
		}
	}
	HeldBodies.Reset();
}
