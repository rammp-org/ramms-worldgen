// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RammsWorldGenBridge.generated.h"

class ARammsWorldGenerator;

/**
 * Static entry points for programs that drive world generation from outside
 * the engine over the Remote Control API (ramms-tools' `ramms_tools.worldgen`
 * calls these on the CDO `/Script/RammsWorldGen.Default__RammsWorldGenBridge`).
 *
 * Every function takes or returns plain strings and numbers so it survives the
 * JSON round trip; generators are addressed by actor path, presets and maps by
 * asset path or short name. The "current world" is the play world if one is
 * running (PIE or game), else the editor world — the same rule RammsCoreBridge
 * uses, so actor paths from the two bridges agree.
 */
UCLASS()
class RAMMSWORLDGEN_API URammsWorldGenBridge : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Actor paths of every ARammsWorldGenerator in the current world. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static TArray<FString> FindGenerators();

	/** Asset paths of every URammsWorldPreset the asset registry knows about. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static TArray<FString> ListPresets();

	/** JSON array describing the presets: name, path, scene kind, description, site size. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static FString DescribePresets();

	/** Asset paths of the example / generated maps under /Game/Maps/Gen (and any map whose name starts with Map_Gen_). */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static TArray<FString> ListGeneratedMaps();

	/**
	 * Spawn a generator in the current world pointed at a preset (asset path or short name such as
	 * "PRE_Downtown") with the given seed. Returns the actor path, or "" on failure. Does not generate.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static FString SpawnGenerator(const FString& Preset, int32 Seed, FVector Location, const FString& Label = TEXT("WorldGenerator"));

	/** Set (or clear with "") the preset of an existing generator. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static bool SetPreset(const FString& GeneratorPath, const FString& Preset);

	/** Regenerate with Seed (Seed < 0 keeps the current seed). Returns false if the generator was not found. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static bool Generate(const FString& GeneratorPath, int32 Seed = -1);

	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static bool Randomize(const FString& GeneratorPath);

	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static bool Cleanup(const FString& GeneratorPath);

	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static bool IsGenerating(const FString& GeneratorPath);

	/**
	 * JSON status of a generator: preset, seed, generating, scene kind, counts of features / routes /
	 * pieces / issues, suggested start, bounds. Cheap enough to poll.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static FString GetStatusJson(const FString& GeneratorPath);

	/** The full ground-truth manifest as JSON ("" if the generator was not found). */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static FString GetManifestJson(const FString& GeneratorPath);

	/** Write the manifest to AbsolutePath ("" = Saved/WorldGen/<name>_<preset>_<seed>.json). Returns the path written. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static FString ExportManifest(const FString& GeneratorPath, const FString& AbsolutePath);

	/** Resolved parameter block (preset + profile + overrides) as a JSON object of name -> number. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static FString GetParamsJson(const FString& GeneratorPath);

	/**
	 * Apply overrides from a JSON object of name -> number (bools as 0/1). Unknown names are reported in
	 * the returned JSON ({"applied": [...], "unknown": [...]}). Takes effect on the next Generate.
	 */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static FString SetParamsJson(const FString& GeneratorPath, const FString& Json);

	/** Drop every per-instance override so the preset's values flow through again. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static bool ClearOverrides(const FString& GeneratorPath);

	/** Move the PlayerStart to the manifest's suggested start. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static bool PlacePlayerStart(const FString& GeneratorPath);

	/**
	 * Open a map: in a running game this travels to it; in the editor it loads it as the current level.
	 * Accepts an asset path (/Game/Maps/Gen/Map_Gen_Downtown) or a short name (Map_Gen_Downtown).
	 */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static bool OpenMap(const FString& Map);

	/** Name of the current world's map (short name) and whether it is a play world; JSON. */
	UFUNCTION(BlueprintCallable, Category = "Ramms|WorldGen")
	static FString GetWorldInfo();

private:
	static UWorld* GetCurrentWorld();
	static ARammsWorldGenerator* Find(const FString& GeneratorPath);
	static FString ResolvePresetPath(const FString& Preset);
	static FString ResolveMapPath(const FString& Map);
};
