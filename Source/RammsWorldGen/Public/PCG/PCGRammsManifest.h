// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "PCG/RammsPCGCommon.h"
#include "PCGRammsManifest.generated.h"

/**
 * Collects route segments and feature points from the other nodes into one
 * JSON manifest (an attribute set with a "Json" attribute) that the generator
 * reads off the graph output. Procedural pieces are not fed here: the
 * generator gathers those from the spawned actors.
 *
 * Pins: Routes (points from Hazard Placer / Indoor Layout), Features (points
 * with ManifestKind etc.: the Features and Props pins of every node); Out.
 */
UCLASS(BlueprintType, ClassGroup = (Procedural))
class RAMMSWORLDGEN_API UPCGRammsManifestSettings : public URammsPCGSettingsBase
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("RammsManifest")); }
	virtual FText GetDefaultNodeTitle() const override { return NSLOCTEXT("RammsPCG", "ManifestTitle", "Ramms: Manifest"); }
	virtual FText GetNodeTooltipText() const override { return NSLOCTEXT("RammsPCG", "ManifestTooltip", "Routes and features to one JSON manifest for the generator; wire Out to the graph Output."); }
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::Param; }
#endif
	virtual bool UseSeed() const override { return false; }

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
};

class FPCGRammsManifestElement : public FRammsPCGElementBase
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};
