// Copyright ATDev. All Rights Reserved.
#include "RammsWorldGenGraphLibrary.h"

#include "Elements/PCGSpawnActor.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "MeshSelectors/PCGMeshSelectorByAttribute.h"
#include "Metadata/PCGAttributePropertySelector.h"
#include "Engine/CollisionProfile.h"
#include "Metadata/PCGObjectPropertyOverride.h"
#include "PCG/RammsPCGCommon.h"
#include "RammsProcPiece.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGInputOutputSettings.h"
#include "UObject/UnrealType.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#if WITH_EDITOR
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "SGraphPanel.h"
#include "EdGraph/EdGraph.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#endif

TArray<FName> URammsWorldGenGraphLibrary::PiecePropertyNames()
{
	using namespace RammsPCG;
	return { KindId, SizeX, SizeY, SizeZ, Rise, Slope, FlareSlope, StepCount, OpeningWidth, OpeningOffset, OpeningHeight,
		LandingSize, WarningDepth, UVScale, MaterialPath, MaterialPath2, MaterialPath3, Openings, ManifestKind, ManifestSubtype, bCompliant, IssueId, Measure, Threshold };
}

void URammsWorldGenGraphLibrary::ConfigurePieceSpawner(UPCGSpawnActorSettings* Settings)
{
	if (!Settings)
	{
		return;
	}
	Settings->Modify();
	Settings->SetTemplateActorClass(ARammsProcPiece::StaticClass());
	Settings->Option = EPCGSpawnActorOption::NoMerging;
	Settings->AttachOptions = EPCGAttachOptions::Attached;
	Settings->PostSpawnFunctionNames = { FName(TEXT("Rebuild")) };
	Settings->TagsToAddOnActors = { FName(TEXT("RammsProcPiece")) };
	Settings->SpawnedActorPropertyOverrideDescriptions.Reset();
	for (const FName& Prop : PiecePropertyNames())
	{
		FPCGAttributePropertyInputSelector Selector;
		Selector.SetAttributeName(Prop);
		Settings->SpawnedActorPropertyOverrideDescriptions.Emplace(Selector, Prop.ToString());
	}
	Settings->MarkPackageDirty();
}

void URammsWorldGenGraphLibrary::ConfigureModuleSpawner(UPCGStaticMeshSpawnerSettings* Settings)
{
	if (!Settings)
	{
		return;
	}
	Settings->Modify();
	Settings->SetMeshSelectorType(UPCGMeshSelectorByAttribute::StaticClass());
	if (UPCGMeshSelectorByAttribute* Sel = Cast<UPCGMeshSelectorByAttribute>(Settings->MeshSelectorParameters))
	{
		Sel->AttributeName = RammsPCG::Mesh;
		// PCG's ISM template defaults to NoCollision: everything generated must be solid (players, the wheelchair
		// and the arm collide with it); meshes without simple collision get complex-as-simple at palette build.
		Sel->TemplateDescriptor.BodyInstance.SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Sel->bUseAttributeMaterialOverrides = true;
		Sel->MaterialOverrideAttributes.Reset();
		for (int32 i = 0; i < RammsPCG::NumModuleMaterials; ++i)
		{
			Sel->MaterialOverrideAttributes.Add(RammsPCG::ModuleMaterial(i));
		}
	}
	Settings->MarkPackageDirty();
}

void URammsWorldGenGraphLibrary::ConfigureFoliageSpawner(UPCGStaticMeshSpawnerSettings* Settings)
{
	if (!Settings)
	{
		return;
	}
	Settings->Modify();
	Settings->SetMeshSelectorType(UPCGMeshSelectorByAttribute::StaticClass());
	if (UPCGMeshSelectorByAttribute* Sel = Cast<UPCGMeshSelectorByAttribute>(Settings->MeshSelectorParameters))
	{
		Sel->AttributeName = RammsPCG::Mesh;
		Sel->TemplateDescriptor.BodyInstance.SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Sel->TemplateDescriptor.InstanceStartCullDistance = 4000;
		Sel->TemplateDescriptor.InstanceEndCullDistance = 7000;
		Sel->TemplateDescriptor.bAffectDistanceFieldLighting = false;
		Sel->TemplateDescriptor.bReceivesDecals = false;
		Sel->bUseAttributeMaterialOverrides = false;
		Sel->MaterialOverrideAttributes.Reset();
	}
	Settings->MarkPackageDirty();
}

void URammsWorldGenGraphLibrary::ConfigureMeshSpawner(UPCGStaticMeshSpawnerSettings* Settings, bool bMaterialFromAttribute)
{
	if (!Settings)
	{
		return;
	}
	Settings->Modify();
	Settings->SetMeshSelectorType(UPCGMeshSelectorByAttribute::StaticClass());
	if (UPCGMeshSelectorByAttribute* Sel = Cast<UPCGMeshSelectorByAttribute>(Settings->MeshSelectorParameters))
	{
		Sel->AttributeName = RammsPCG::Mesh;
		Sel->TemplateDescriptor.BodyInstance.SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Sel->bUseAttributeMaterialOverrides = bMaterialFromAttribute;
		Sel->MaterialOverrideAttributes.Reset();
		if (bMaterialFromAttribute)
		{
			Sel->MaterialOverrideAttributes.Add(RammsPCG::Material);
		}
	}
	Settings->MarkPackageDirty();
}

namespace
{
	void ResetIOPins(UPCGNode* Node, const TArray<FName>& Labels)
	{
		UPCGGraphInputOutputSettings* IO = Node ? Cast<UPCGGraphInputOutputSettings>(Node->GetSettings()) : nullptr;
		if (!IO)
		{
			return;
		}
		IO->Modify();
		// Pins is protected; empty it through reflection, then add through the public API.
		if (FArrayProperty* Prop = FindFProperty<FArrayProperty>(UPCGGraphInputOutputSettings::StaticClass(), TEXT("Pins")))
		{
			FScriptArrayHelper Helper(Prop, Prop->ContainerPtrToValuePtr<void>(IO));
			Helper.EmptyValues();
		}
		for (const FName& Label : Labels)
		{
			(void)IO->AddPin(FPCGPinProperties(Label, FPCGDataTypeIdentifier{EPCGDataType::Any}));
		}
		Node->UpdateAfterSettingsChangeDuringCreation();
	}
}

void URammsWorldGenGraphLibrary::SetGraphPins(UPCGGraph* Graph, const TArray<FName>& InputPins, const TArray<FName>& OutputPins)
{
	if (!Graph)
	{
		return;
	}
	Graph->Modify();
	ResetIOPins(Graph->GetInputNode(), InputPins);
	ResetIOPins(Graph->GetOutputNode(), OutputPins);
	Graph->MarkPackageDirty();
}

void URammsWorldGenGraphLibrary::SetNodeComment(UPCGNode* Node, const FString& Comment)
{
#if WITH_EDITORONLY_DATA
	if (!Node)
	{
		return;
	}
	Node->Modify();
	Node->NodeComment = Comment;
	Node->bCommentBubblePinned = !Comment.IsEmpty();
	Node->bCommentBubbleVisible = !Comment.IsEmpty();
#endif
}

void URammsWorldGenGraphLibrary::ClearGraphComments(UPCGGraph* Graph)
{
#if WITH_EDITOR
	if (Graph)
	{
		Graph->Modify();
		Graph->SetCommentNodes({});
	}
#endif
}

void URammsWorldGenGraphLibrary::AddGraphComment(UPCGGraph* Graph, const FString& Text, int32 X, int32 Y, int32 Width, int32 Height, FLinearColor Color, int32 FontSize)
{
#if WITH_EDITOR
	if (!Graph)
	{
		return;
	}
	TArray<FPCGGraphCommentNodeData> Comments = Graph->GetCommentNodes();
	FPCGGraphCommentNodeData& C = Comments.AddDefaulted_GetRef();
	C.NodePosX = X;
	C.NodePosY = Y;
	C.NodeWidth = Width;
	C.NodeHeight = Height;
	C.NodeComment = Text;
	C.CommentColor = Color;
	C.FontSize = FontSize;
	// The box title is enough; no zoomed-out bubble repeating it.
	C.bColorCommentBubble = 0;
	C.bCommentBubbleVisible_InDetailsPanel = 0;
	C.bCommentBubbleVisible = 0;
	C.bCommentBubblePinned = 0;
	C.CommentDepth = -1;
	C.GUID = FGuid::NewGuid();
	Graph->Modify();
	Graph->SetCommentNodes(MoveTemp(Comments));
#endif
}

void URammsWorldGenGraphLibrary::RefreshNodePins(UPCGNode* Node)
{
	if (Node)
	{
		Node->UpdateAfterSettingsChangeDuringCreation();
	}
}

#if WITH_EDITOR
namespace
{
	TSharedPtr<SGraphPanel> FindPanel(const TSharedRef<SWidget>& Widget, const UPCGGraph* Graph)
	{
		const FString Type = Widget->GetTypeAsString();
		if (Type == TEXT("SGraphPanel"))
		{
			TSharedRef<SGraphPanel> Panel = StaticCastSharedRef<SGraphPanel>(Widget);
			const UEdGraph* Ed = Panel->GetGraphObj();
			if (Ed && Ed->GetTypedOuter<UPCGGraph>() == Graph)
			{
				return Panel;
			}
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 i = 0; Children && i < Children->Num(); ++i)
		{
			if (TSharedPtr<SGraphPanel> Found = FindPanel(Children->GetChildAt(i), Graph))
			{
				return Found;
			}
		}
		return nullptr;
	}

	TSharedPtr<SGraphPanel> FindPanel(const UPCGGraph* Graph)
	{
		if (!Graph || !FSlateApplication::IsInitialized())
		{
			return nullptr;
		}
		// Asset editors usually float in their own window, a child of the main window.
		TArray<TSharedRef<SWindow>> Windows;
		FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
		for (const TSharedRef<SWindow>& Window : Windows)
		{
			if (TSharedPtr<SGraphPanel> Found = FindPanel(Window, Graph))
			{
				return Found;
			}
		}
		return nullptr;
	}
}
#endif

FIntPoint URammsWorldGenGraphLibrary::SetGraphEditorView(UPCGGraph* Graph, FVector2D TopLeft, float Zoom)
{
#if WITH_EDITOR
	if (TSharedPtr<SGraphPanel> Panel = FindPanel(Graph))
	{
		Panel->RestoreViewSettings(FVector2f(TopLeft), Zoom);
		const FVector2f Size = Panel->GetPaintSpaceGeometry().GetLocalSize();
		return FIntPoint(FMath::RoundToInt(Size.X), FMath::RoundToInt(Size.Y));
	}
#endif
	return FIntPoint::ZeroValue;
}

bool URammsWorldGenGraphLibrary::CaptureGraphEditor(UPCGGraph* Graph, const FString& PngPath)
{
#if WITH_EDITOR
	TSharedPtr<SGraphPanel> Panel = FindPanel(Graph);
	if (!Panel)
	{
		return false;
	}
	TArray<FColor> Pixels;
	FIntVector Size;
	if (!FSlateApplication::Get().TakeScreenshot(Panel.ToSharedRef(), Pixels, Size) || Size.X <= 0 || Size.Y <= 0)
	{
		return false;
	}
	for (FColor& C : Pixels)
	{
		C.A = 255;
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
	return FFileHelper::SaveArrayToFile(Png, *PngPath);
#else
	return false;
#endif
}

// --- glass ------------------------------------------------------------------------------------------------

bool URammsWorldGenGraphLibrary::UsesRammsGlass(const UPrimitiveComponent* Component)
{
	if (!Component)
	{
		return false;
	}
	for (int32 i = 0; i < Component->GetNumMaterials(); ++i)
	{
		const UMaterialInterface* MI = Component->GetMaterial(i);
		const UMaterial* Base = MI ? MI->GetMaterial() : nullptr;
		if (Base && Base->GetName().StartsWith(TEXT("M_RammsGlass")))
		{
			return true;
		}
	}
	return false;
}

bool URammsWorldGenGraphLibrary::SetGlassInstanceLook(UInstancedStaticMeshComponent* Component, int32 InstanceIndex, float Frost, float OpacityScale,
	FLinearColor Tint, float Bump, bool bMarkRenderStateDirty)
{
	if (!Component || !Component->IsValidInstance(InstanceIndex))
	{
		return false;
	}
	if (Component->NumCustomDataFloats < GlassCustomDataFloats)
	{
		Component->SetNumCustomDataFloats(GlassCustomDataFloats);
	}
	const float Data[GlassCustomDataFloats] = { Frost, OpacityScale, Tint.R, Tint.G, Tint.B, Bump };
	for (int32 i = 0; i < GlassCustomDataFloats; ++i)
	{
		Component->SetCustomDataValue(InstanceIndex, i, Data[i], false);
	}
	if (bMarkRenderStateDirty)
	{
		Component->MarkRenderStateDirty();
	}
	return true;
}

namespace
{
	// Glass tints as multipliers on the material's own (mostly neutral) tint: float-glass green, blue, bronze, smoked grey.
	const FLinearColor GlassTints[] = {
		FLinearColor(0.70f, 1.10f, 0.85f),
		FLinearColor(0.60f, 0.85f, 1.25f),
		FLinearColor(1.25f, 0.90f, 0.55f),
		FLinearColor(0.45f, 0.45f, 0.48f),
	};
}

int32 URammsWorldGenGraphLibrary::VaryGlassInstances(AActor* Actor, int32 Seed, float Variation, float FrostChance, float TintChance, float BumpChance)
{
	if (!Actor)
	{
		return 0;
	}
	const float V = FMath::Clamp(Variation, 0.f, 1.f);
	int32 Changed = 0;
	TArray<UInstancedStaticMeshComponent*> Components;
	Actor->GetComponents<UInstancedStaticMeshComponent>(Components);
	for (UInstancedStaticMeshComponent* ISM : Components)
	{
		if (!ISM || ISM->GetInstanceCount() == 0 || !UsesRammsGlass(ISM))
		{
			continue;
		}
		if (V <= 0.f)
		{
			if (ISM->NumCustomDataFloats > 0)
			{
				ISM->SetNumCustomDataFloats(0);
				ISM->MarkRenderStateDirty();
			}
			continue;
		}
		for (int32 i = 0; i < ISM->GetInstanceCount(); ++i)
		{
			FTransform T;
			ISM->GetInstanceTransform(i, T, true);
			// one stream per placement (10 cm grid), so every glass component of one shelter or car agrees and
			// the same seed gives the same look
			const FIntVector Q(FMath::RoundToInt(T.GetLocation().X / 10.0), FMath::RoundToInt(T.GetLocation().Y / 10.0), FMath::RoundToInt(T.GetLocation().Z / 10.0));
			FRandomStream R(static_cast<int32>(HashCombine(HashCombine(HashCombine(GetTypeHash(Seed), GetTypeHash(Q.X)), HashCombine(GetTypeHash(Q.Y), GetTypeHash(Q.Z))), 0x9a55u)));
			// draw everything up front so changing one chance does not reshuffle the others
			const float RFrost = R.FRand(), RFrostAmt = R.FRand(), ROpac = R.FRand(), RTint = R.FRand(), RTintPick = R.FRand(), RBump = R.FRand(), RBumpAmt = R.FRand();
			const float Frost = RFrost < FrostChance ? V * FMath::Lerp(0.35f, 1.f, RFrostAmt) : 0.f;
			const float Opacity = 1.f + V * 0.25f * (2.f * ROpac - 1.f);
			FLinearColor Tint = FLinearColor::White;
			if (RTint < TintChance)
			{
				const int32 Pick = FMath::Min(static_cast<int32>(RTintPick * UE_ARRAY_COUNT(GlassTints)), static_cast<int32>(UE_ARRAY_COUNT(GlassTints)) - 1);
				Tint = FMath::Lerp(FLinearColor::White, GlassTints[Pick], V);
			}
			const float Bump = RBump < BumpChance ? V * FMath::Lerp(0.3f, 1.f, RBumpAmt) : 0.f;
			Changed += SetGlassInstanceLook(ISM, i, Frost, Opacity, Tint, Bump, false) ? 1 : 0;
		}
		ISM->MarkRenderStateDirty();
	}
	return Changed;
}

AActor* URammsWorldGenGraphLibrary::SpawnInstancedMesh(UObject* WorldContextObject, UStaticMesh* Mesh, const TArray<FTransform>& Transforms,
	const TArray<float>& CustomData, int32 NumCustomDataFloats, FName Tag)
{
	UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	if (!World || !Mesh)
	{
		return nullptr;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SP);
	if (!Actor)
	{
		return nullptr;
	}
	UInstancedStaticMeshComponent* ISM = NewObject<UInstancedStaticMeshComponent>(Actor, TEXT("Instances"), RF_Transactional);
	ISM->SetMobility(EComponentMobility::Movable);
	Actor->SetRootComponent(ISM);
	Actor->AddInstanceComponent(ISM);
	ISM->SetStaticMesh(Mesh);
	ISM->RegisterComponent();
	const int32 N = FMath::Max(NumCustomDataFloats, 0);
	if (N > 0)
	{
		ISM->SetNumCustomDataFloats(N);
	}
	for (int32 i = 0; i < Transforms.Num(); ++i)
	{
		const int32 Index = ISM->AddInstance(Transforms[i], true);
		for (int32 k = 0; k < N && i * N + k < CustomData.Num(); ++k)
		{
			ISM->SetCustomDataValue(Index, k, CustomData[i * N + k], false);
		}
	}
	ISM->MarkRenderStateDirty();
	if (!Tag.IsNone())
	{
		Actor->Tags.Add(Tag);
	}
#if WITH_EDITOR
	Actor->SetActorLabel(Tag.IsNone() ? FString(TEXT("RammsInstancedMesh")) : Tag.ToString());
#endif
	return Actor;
}
