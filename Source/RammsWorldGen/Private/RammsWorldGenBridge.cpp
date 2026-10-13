// Copyright ATDev. All Rights Reserved.
#include "RammsWorldGenBridge.h"

#include "RammsWorldGenerator.h"
#include "RammsWorldGenModule.h"
#include "RammsWorldPreset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"
#if WITH_EDITOR
#include "Editor.h"
#include "FileHelpers.h"
#endif

namespace
{
	FString ToJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	FString ToJson(const TArray<TSharedPtr<FJsonValue>>& Arr)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Arr, Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> VecJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y); O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	FString SceneKindName(ERammsSceneKind Kind)
	{
		return StaticEnum<ERammsSceneKind>()->GetNameStringByValue(static_cast<int64>(Kind));
	}
}

UWorld* URammsWorldGenBridge::GetCurrentWorld()
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if ((Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game) && Context.World())
		{
			return Context.World();
		}
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Editor && Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}

ARammsWorldGenerator* URammsWorldGenBridge::Find(const FString& GeneratorPath)
{
	UWorld* World = GetCurrentWorld();
	if (!World)
	{
		return nullptr;
	}
	ARammsWorldGenerator* First = nullptr;
	for (TActorIterator<ARammsWorldGenerator> It(World); It; ++It)
	{
		ARammsWorldGenerator* Gen = *It;
		if (!IsValid(Gen))
		{
			continue;
		}
		if (GeneratorPath.IsEmpty())
		{
			return Gen; // "" = the first (usually only) generator
		}
		if (Gen->GetPathName() == GeneratorPath || Gen->GetName() == GeneratorPath)
		{
			return Gen;
		}
#if WITH_EDITOR
		if (Gen->GetActorLabel() == GeneratorPath)
		{
			return Gen;
		}
#endif
		First = First ? First : Gen;
	}
	return nullptr;
}

FString URammsWorldGenBridge::ResolvePresetPath(const FString& Preset)
{
	if (Preset.IsEmpty())
	{
		return FString();
	}
	if (Preset.StartsWith(TEXT("/")))
	{
		return Preset;
	}
	for (const FString& Path : ListPresets())
	{
		if (FPackageName::GetShortName(Path) == Preset || Path.EndsWith(TEXT("/") + Preset) || Path.EndsWith(TEXT(".") + Preset))
		{
			return Path;
		}
	}
	return FString::Printf(TEXT("/Game/WorldGen/Presets/%s"), *Preset);
}

FString URammsWorldGenBridge::ResolveMapPath(const FString& Map)
{
	if (Map.StartsWith(TEXT("/")))
	{
		return Map;
	}
	for (const FString& Path : ListGeneratedMaps())
	{
		if (FPackageName::GetShortName(Path) == Map)
		{
			return Path;
		}
	}
	return FString::Printf(TEXT("/Game/Maps/Gen/%s"), *Map);
}

// --- discovery ---------------------------------------------------------------------------

TArray<FString> URammsWorldGenBridge::FindGenerators()
{
	TArray<FString> Out;
	if (UWorld* World = GetCurrentWorld())
	{
		for (TActorIterator<ARammsWorldGenerator> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				Out.Add(It->GetPathName());
			}
		}
	}
	return Out;
}

TArray<FString> URammsWorldGenBridge::ListPresets()
{
	TArray<FString> Out;
	const FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByClass(URammsWorldPreset::StaticClass()->GetClassPathName(), Assets, true);
	for (const FAssetData& A : Assets)
	{
		Out.Add(A.GetObjectPathString());
	}
	// Presets already in memory but not yet scanned (freshly built in this session).
	for (TObjectIterator<URammsWorldPreset> It; It; ++It)
	{
		if (It->IsAsset())
		{
			Out.AddUnique(It->GetPathName());
		}
	}
	Out.Sort();
	return Out;
}

FString URammsWorldGenBridge::DescribePresets()
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& Path : ListPresets())
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), FPackageName::GetShortName(Path).Replace(TEXT("."), TEXT("")));
		O->SetStringField(TEXT("path"), Path);
		if (const URammsWorldPreset* P = LoadObject<URammsWorldPreset>(nullptr, *Path))
		{
			O->SetStringField(TEXT("name"), P->GetName());
			O->SetStringField(TEXT("scene_kind"), SceneKindName(P->SceneKind));
			O->SetStringField(TEXT("description"), P->Description.ToString());
			O->SetNumberField(TEXT("site_size_x"), P->Params.SiteSizeX);
			O->SetNumberField(TEXT("site_size_y"), P->Params.SiteSizeY);
			O->SetStringField(TEXT("palette"), P->Palette.ToSoftObjectPath().ToString());
			O->SetStringField(TEXT("graph"), P->Graph.ToSoftObjectPath().ToString());
		}
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	return ToJson(Arr);
}

TArray<FString> URammsWorldGenBridge::ListGeneratedMaps()
{
	TArray<FString> Out;
	const FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByClass(UWorld::StaticClass()->GetClassPathName(), Assets, false);
	for (const FAssetData& A : Assets)
	{
		const FString Package = A.PackageName.ToString();
		if (Package.StartsWith(TEXT("/Game/Maps/Gen/")) || A.AssetName.ToString().StartsWith(TEXT("Map_Gen_")))
		{
			Out.Add(Package);
		}
	}
	Out.Sort();
	return Out;
}

FString URammsWorldGenBridge::GetWorldInfo()
{
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	UWorld* World = GetCurrentWorld();
	O->SetBoolField(TEXT("ok"), World != nullptr);
	if (World)
	{
		O->SetStringField(TEXT("map"), World->GetMapName());
		O->SetStringField(TEXT("package"), World->GetOutermost()->GetName());
		O->SetBoolField(TEXT("play"), World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
		O->SetStringField(TEXT("world_type"), World->WorldType == EWorldType::PIE ? TEXT("PIE") : World->WorldType == EWorldType::Game ? TEXT("Game") : TEXT("Editor"));
		O->SetNumberField(TEXT("generators"), FindGenerators().Num());
	}
	return ToJson(O.ToSharedRef());
}

// --- lifecycle ---------------------------------------------------------------------------

FString URammsWorldGenBridge::SpawnGenerator(const FString& Preset, int32 Seed, FVector Location, const FString& Label)
{
	UWorld* World = GetCurrentWorld();
	if (!World)
	{
		return FString();
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ARammsWorldGenerator* Gen = World->SpawnActor<ARammsWorldGenerator>(ARammsWorldGenerator::StaticClass(), FTransform(Location), SP);
	if (!Gen)
	{
		return FString();
	}
#if WITH_EDITOR
	if (!Label.IsEmpty())
	{
		Gen->SetActorLabel(Label);
	}
#endif
	Gen->Seed = Seed;
	if (!Preset.IsEmpty())
	{
		SetPreset(Gen->GetPathName(), Preset);
	}
	return Gen->GetPathName();
}

bool URammsWorldGenBridge::SetPreset(const FString& GeneratorPath, const FString& Preset)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	if (!Gen)
	{
		return false;
	}
	if (Preset.IsEmpty())
	{
		Gen->Preset = nullptr;
	}
	else
	{
		URammsWorldPreset* P = LoadObject<URammsWorldPreset>(nullptr, *ResolvePresetPath(Preset));
		if (!P)
		{
			UE_LOG(LogRammsWorldGen, Warning, TEXT("SetPreset: preset '%s' not found"), *Preset);
			return false;
		}
		Gen->Preset = P;
	}
	Gen->ResolveParams();
	return true;
}

bool URammsWorldGenBridge::Generate(const FString& GeneratorPath, int32 Seed)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	if (!Gen)
	{
		return false;
	}
	if (Seed >= 0)
	{
		Gen->RegenerateWithSeed(Seed);
	}
	else
	{
		Gen->Regenerate();
	}
	return true;
}

bool URammsWorldGenBridge::Randomize(const FString& GeneratorPath)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	if (!Gen) { return false; }
	Gen->Randomize();
	return true;
}

bool URammsWorldGenBridge::Cleanup(const FString& GeneratorPath)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	if (!Gen) { return false; }
	Gen->Cleanup();
	return true;
}

bool URammsWorldGenBridge::IsGenerating(const FString& GeneratorPath)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	return Gen && Gen->IsGenerating();
}

bool URammsWorldGenBridge::PlacePlayerStart(const FString& GeneratorPath)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	if (!Gen) { return false; }
	Gen->PlacePlayerStart();
	return true;
}

bool URammsWorldGenBridge::ClearOverrides(const FString& GeneratorPath)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	if (!Gen) { return false; }
	Gen->OverrideKeys.Empty();
	Gen->ParamOverrides = FRammsWorldGenParams();
	Gen->ResolveParams();
	return true;
}

// --- state -------------------------------------------------------------------------------

FString URammsWorldGenBridge::GetStatusJson(const FString& GeneratorPath)
{
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	O->SetBoolField(TEXT("ok"), Gen != nullptr);
	if (!Gen)
	{
		O->SetStringField(TEXT("error"), GeneratorPath.IsEmpty() ? TEXT("no generator in the current world") : TEXT("generator not found: ") + GeneratorPath);
		return ToJson(O.ToSharedRef());
	}
	const FRammsWorldManifest& M = Gen->Manifest;
	O->SetStringField(TEXT("path"), Gen->GetPathName());
	O->SetStringField(TEXT("name"), Gen->GetName());
#if WITH_EDITOR
	O->SetStringField(TEXT("label"), Gen->GetActorLabel());
#endif
	O->SetStringField(TEXT("preset"), Gen->Preset ? Gen->Preset->GetPathName() : FString());
	O->SetStringField(TEXT("scene_kind"), SceneKindName(Gen->Preset ? Gen->Preset->SceneKind : ERammsSceneKind::Downtown));
	O->SetNumberField(TEXT("seed"), Gen->Seed);
	O->SetBoolField(TEXT("generating"), Gen->IsGenerating());
	O->SetBoolField(TEXT("has_manifest"), M.Features.Num() > 0 || M.Routes.Num() > 0);
	O->SetNumberField(TEXT("manifest_seed"), M.Seed);
	O->SetNumberField(TEXT("features"), M.Features.Num());
	O->SetNumberField(TEXT("routes"), M.Routes.Num());
	O->SetNumberField(TEXT("issues"), M.CountIssues());
	TArray<FString> Keys;
	for (const FName& K : Gen->OverrideKeys) { Keys.Add(K.ToString()); }
	TArray<TSharedPtr<FJsonValue>> KeyArr;
	for (const FString& K : Keys) { KeyArr.Add(MakeShared<FJsonValueString>(K)); }
	O->SetArrayField(TEXT("override_keys"), KeyArr);
	O->SetObjectField(TEXT("site_size"), VecJson(FVector(Gen->Params.SiteSizeX, Gen->Params.SiteSizeY, 0.0)));
	O->SetObjectField(TEXT("location"), VecJson(Gen->GetActorLocation()));
	if (M.Bounds.IsValid)
	{
		O->SetObjectField(TEXT("bounds_min"), VecJson(M.Bounds.Min));
		O->SetObjectField(TEXT("bounds_max"), VecJson(M.Bounds.Max));
	}
	O->SetObjectField(TEXT("suggested_start"), VecJson(M.SuggestedStart.GetLocation()));
	O->SetNumberField(TEXT("suggested_start_yaw"), M.SuggestedStart.Rotator().Yaw);
	return ToJson(O.ToSharedRef());
}

FString URammsWorldGenBridge::GetManifestJson(const FString& GeneratorPath)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	return Gen ? Gen->GetManifestJson() : FString();
}

FString URammsWorldGenBridge::ExportManifest(const FString& GeneratorPath, const FString& AbsolutePath)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	return Gen ? Gen->ExportManifest(AbsolutePath) : FString();
}

FString URammsWorldGenBridge::GetParamsJson(const FString& GeneratorPath)
{
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	if (!Gen)
	{
		return FString();
	}
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	for (const FName& Name : Gen->GetParamNames())
	{
		double V = 0.0;
		if (Gen->GetParam(Name, V))
		{
			O->SetNumberField(Name.ToString(), V);
		}
	}
	O->SetStringField(TEXT("Palette"), Gen->Params.Palette.ToSoftObjectPath().ToString());
	return ToJson(O.ToSharedRef());
}

FString URammsWorldGenBridge::SetParamsJson(const FString& GeneratorPath, const FString& Json)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Applied, Unknown;
	ARammsWorldGenerator* Gen = Find(GeneratorPath);
	Result->SetBoolField(TEXT("ok"), Gen != nullptr);
	if (Gen)
	{
		TSharedPtr<FJsonObject> In;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, In) || !In.IsValid())
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("error"), TEXT("not a JSON object"));
		}
		else
		{
			for (const auto& Pair : In->Values)
			{
				const FString Key(Pair.Key);
				double V = 0.0;
				bool bNumber = Pair.Value->TryGetNumber(V);
				if (!bNumber)
				{
					bool B = false;
					if (Pair.Value->TryGetBool(B)) { V = B ? 1.0 : 0.0; bNumber = true; }
				}
				if (bNumber && Gen->SetParam(FName(*Key), V))
				{
					Applied.Add(MakeShared<FJsonValueString>(Key));
				}
				else
				{
					Unknown.Add(MakeShared<FJsonValueString>(Key));
				}
			}
		}
	}
	Result->SetArrayField(TEXT("applied"), Applied);
	Result->SetArrayField(TEXT("unknown"), Unknown);
	return ToJson(Result.ToSharedRef());
}

// --- maps --------------------------------------------------------------------------------

bool URammsWorldGenBridge::OpenMap(const FString& Map)
{
	const FString Path = ResolveMapPath(Map);
	UWorld* World = GetCurrentWorld();
	if (!World)
	{
		return false;
	}
	if (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game)
	{
		UGameplayStatics::OpenLevel(World, FName(*Path));
		return true;
	}
#if WITH_EDITOR
	return static_cast<bool>(FEditorFileUtils::LoadMap(Path, false, true));
#else
	return false;
#endif
}
