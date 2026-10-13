// Copyright ATDev. All Rights Reserved.
#include "RammsWorldGenerator.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGGraph.h"
#include "PCGParamData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/PCGMetadataAttributeTraits.h"
#include "RammsAccessibilityProfile.h"
#include "RammsProcPiece.h"
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
}

// --- params ------------------------------------------------------------------------

void ARammsWorldGenerator::ResolveParams()
{
	if (Preset)
	{
		Params = Preset->ResolveParams();
		if (Params.Palette.IsNull())
		{
			Params.Palette = Preset->Palette;
		}
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

void ARammsWorldGenerator::Regenerate()
{
	if (!PCGComponent)
	{
		return;
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
		return;
	}

	EnsureBound();
	Manifest = FRammsWorldManifest();
	Manifest.Seed = Seed;
	Manifest.Preset = Preset ? Preset->GetFName() : NAME_None;
	Manifest.SceneKind = Preset ? Preset->SceneKind : ERammsSceneKind::Downtown;

	UE_LOG(LogRammsWorldGen, Log, TEXT("%s: generating preset=%s seed=%d site=%.0fx%.0f"), *GetName(), *Manifest.Preset.ToString(), Seed, Params.SiteSizeX, Params.SiteSizeY);
	PCGComponent->GenerateLocal(/*bForce=*/true);
}

void ARammsWorldGenerator::RegenerateWithSeed(int32 NewSeed)
{
	Seed = NewSeed;
	Regenerate();
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
}

bool ARammsWorldGenerator::IsGenerating() const
{
	return PCGComponent && PCGComponent->IsGenerating();
}

void ARammsWorldGenerator::HandleGenerated(UPCGComponent* Component)
{
	BuildManifestFromOutput();
	CollectProcPieces();
	Manifest.Bounds = SiteBounds ? SiteBounds->Bounds.GetBox() : FBox(ForceInit);
	UE_LOG(LogRammsWorldGen, Log, TEXT("%s: generated seed=%d features=%d routes=%d issues=%d"), *GetName(), Seed, Manifest.Features.Num(), Manifest.Routes.Num(), Manifest.CountIssues());
	OnWorldGenerated.Broadcast(Manifest);
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
		const bool bOurs = Piece->ActorHasTag(Tag) || Piece->GetAttachParentActor() == this || Piece->GetOwner() == this
			|| (Piece->ActorHasTag(FName(TEXT("RammsProcPiece"))) && SiteBounds && SiteBounds->Bounds.GetBox().ExpandBy(500.0).IsInside(Piece->GetActorLocation()));
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
