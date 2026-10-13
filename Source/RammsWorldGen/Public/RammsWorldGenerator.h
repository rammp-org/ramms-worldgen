// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RammsWorldGenTypes.h"
#include "RammsWorldGenerator.generated.h"

class UPCGComponent;
class UBoxComponent;
class URammsWorldPreset;
class URammsWorldPalette;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRammsWorldGenerated, const FRammsWorldManifest&, Manifest);

/**
 * Places one generated world. Pick a preset and a seed; the actor sizes its
 * bounds to the preset, flattens preset + profile into Params (which the PCG
 * graph reads with a single Get Actor Property node), runs the graph, and
 * assembles the ground-truth manifest from the graph's output.
 *
 * Regenerate(seed) is the whole runtime API: an episode runner calls it, waits
 * for OnWorldGenerated, reads the manifest, and places the robot at
 * Manifest.SuggestedStart.
 */
UCLASS(BlueprintType, Blueprintable, ClassGroup = (Ramms), meta = (DisplayName = "Ramms World Generator"))
class RAMMSWORLDGEN_API ARammsWorldGenerator : public AActor
{
	GENERATED_BODY()

public:
	ARammsWorldGenerator();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "World Generator")
	TObjectPtr<UPCGComponent> PCGComponent;

	/** Sized to Params.SiteSizeX/Y so the PCG component's bounds match the site. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "World Generator")
	TObjectPtr<UBoxComponent> SiteBounds;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Generator")
	TObjectPtr<URammsWorldPreset> Preset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Generator")
	int32 Seed = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Generator")
	bool bGenerateOnBeginPlay = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Generator")
	bool bRandomizeSeedOnBeginPlay = false;

	/** Per-instance tweaks applied on top of the preset's params before generation. Only members you
	 *  list in OverrideKeys take effect, so a preset update still flows through. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Generator|Overrides")
	FRammsWorldGenParams ParamOverrides;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Generator|Overrides")
	TSet<FName> OverrideKeys;

	/** The resolved parameter block the graph reads. Rebuilt by ResolveParams(); edit the preset or the overrides instead. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "World Generator|Resolved")
	FRammsWorldGenParams Params;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "World Generator|Resolved")
	FRammsWorldManifest Manifest;

	UPROPERTY(BlueprintAssignable, Category = "World Generator")
	FOnRammsWorldGenerated OnWorldGenerated;

	// --- actions ---------------------------------------------------------------------
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "World Generator")
	void Regenerate();

	UFUNCTION(BlueprintCallable, Category = "World Generator")
	void RegenerateWithSeed(int32 NewSeed);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "World Generator")
	void Randomize();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "World Generator")
	void Cleanup();

	/** Resolve preset + profile + overrides into Params and resize the bounds. Called by Regenerate. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "World Generator")
	void ResolveParams();

	/** Move the level's first PlayerStart (or spawn one) to the manifest's suggested start. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "World Generator")
	void PlacePlayerStart();

	UFUNCTION(BlueprintPure, Category = "World Generator")
	FString GetManifestJson() const;

	/** Write the manifest JSON next to the project's Saved folder (or to AbsolutePath when given). Returns the path written. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "World Generator")
	FString ExportManifest(const FString& AbsolutePath);

	UFUNCTION(BlueprintPure, Category = "World Generator")
	bool IsGenerating() const;

	/** Set one parameter by name on the overrides (and record the key). Lets scripts and MCP tools tweak without knowing the struct. Returns false for unknown names. */
	UFUNCTION(BlueprintCallable, Category = "World Generator")
	bool SetParam(FName Name, double Value);

	UFUNCTION(BlueprintPure, Category = "World Generator")
	bool GetParam(FName Name, double& OutValue) const;

	UFUNCTION(BlueprintPure, Category = "World Generator")
	TArray<FName> GetParamNames() const;

	virtual void BeginPlay() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostInitializeComponents() override;

private:
	void EnsureBound();
	void HandleGenerated(UPCGComponent* Component);
	void BuildManifestFromOutput();
	void CollectProcPieces();
	bool bBoundDelegate = false;
};
