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

	/** In a play world (PIE / game): hold the player pawns still while the world is rebuilt (their simulated
	 *  bodies go kinematic, so they neither fall through the old floor nor get shoved by new geometry), then put
	 *  them at the new world's suggested start, at rest. Also moves the PlayerStart there. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Generator|Play")
	bool bPlacePlayersOnGenerate = true;

	/** Height added to the suggested start for placed pawns (cm). The start is already 8 cm above the route. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Generator|Play", meta = (EditCondition = "bPlacePlayersOnGenerate"))
	double PlayerPlacementLift = 2.0;

	/** Seconds after placement before the pawns' physics is switched back on (lets the new collision settle). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "World Generator|Play", meta = (EditCondition = "bPlacePlayersOnGenerate"))
	float PlayerReleaseDelay = 0.2f;

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
	/** Start a generation. Returns false when nothing could be scheduled (no graph): no OnWorldGenerated follows. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "World Generator")
	bool Regenerate();

	UFUNCTION(BlueprintCallable, Category = "World Generator")
	bool RegenerateWithSeed(int32 NewSeed);

	/** Generate with another palette (an asset path or a name under /RammsWorldGen/Palettes, e.g. "PAL_UrbanVerse");
	 *  empty goes back to the preset's. Takes effect at the next Regenerate. Returns false if it does not load. */
	UFUNCTION(BlueprintCallable, Category = "World Generator")
	bool SetPalette(const FString& Palette);

	/** The palette the next (or last) generation uses, as an object path. */
	UFUNCTION(BlueprintPure, Category = "World Generator")
	FString GetPalettePath() const;

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

	/** Put every player pawn at the suggested start now (teleport, velocities zeroed). Play worlds only. */
	UFUNCTION(BlueprintCallable, Category = "World Generator|Play")
	void PlacePlayers();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostInitializeComponents() override;

private:
	void EnsureBound();
	void HandleGenerated(UPCGComponent* Component);
	void HandleCancelled(UPCGComponent* Component);
	void BuildManifestFromOutput();
	void CollectProcPieces();
	void HoldPlayers();
	void ReleasePlayers();
	bool bBoundDelegate = false;

	/** A simulated body of a held pawn and its transform relative to the pawn, for re-placing it. Skeletal
	 *  meshes that simulated only some bodies remember which, so releasing does not ragdoll the rest. */
	struct FHeldBody
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		FTransform RelativeToActor;
		bool bAllBodies = true;
		TArray<FName> SimulatedBones;
	};
	TArray<FHeldBody> HeldBodies;
	FTimerHandle ReleaseTimer;
	FTimerHandle HoldSafetyTimer;
	/** Another generator took over the players while this one was generating: do not place them when done. */
	bool bPlayersTaken = false;
};
