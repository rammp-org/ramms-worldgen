// Copyright ATDev. All Rights Reserved.
#include "PCG/PCGRammsManifest.h"

#include "JsonObjectConverter.h"
#include "PCGContext.h"
#include "PCGPin.h"
#include "Data/PCGBasePointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"

#define LOCTEXT_NAMESPACE "RammsPCG"

TArray<FPCGPinProperties> UPCGRammsManifestSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(RammsPCG::PinRoutes, EPCGDataType::Point, true, true, LOCTEXT("ManifestRoutes", "Route segment points (Hazard Placer, Indoor Layout)"));
	Pins.Emplace(RammsPCG::PinFeatures, EPCGDataType::Point, true, true, LOCTEXT("ManifestFeatures", "Feature points with ManifestKind (Features and Props pins of every node)"));
	return Pins;
}

TArray<FPCGPinProperties> UPCGRammsManifestSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(RammsPCG::PinOut, EPCGDataType::Param);
	return Pins;
}

FPCGElementPtr UPCGRammsManifestSettings::CreateElement() const
{
	return MakeShared<FPCGRammsManifestElement>();
}

namespace
{
	struct FReader
	{
		const UPCGBasePointData* Data = nullptr;
		int32 Num = 0;
		explicit FReader(const UPCGBasePointData* In) : Data(In)
		{
			if (Data)
			{
				Num = Data->GetNumPoints();
			}
		}
		// Ranges are re-fetched per access: cheap views, and nothing here needs a default constructor.
		FTransform T(int32 i) const { return Data->GetConstTransformValueRange()[i]; }
		FVector Min(int32 i) const { return Data->GetConstBoundsMinValueRange()[i]; }
		FVector Max(int32 i) const { return Data->GetConstBoundsMaxValueRange()[i]; }
		int64 Entry(int32 i) const { return Data->GetConstMetadataEntryValueRange()[i]; }
		template <typename T> T Get(int32 i, FName Attr, const T& Default) const
		{
			if (!Data || !Data->Metadata) { return Default; }
			const FPCGMetadataAttribute<T>* A = Data->Metadata->GetConstTypedAttribute<T>(Attr);
			return A ? A->GetValueFromItemKey(Entry(i)) : Default;
		}
		double GetD(int32 i, FName Attr, double Default) const
		{
			if (!Data || !Data->Metadata) { return Default; }
			const FPCGMetadataAttributeBase* B = Data->Metadata->GetConstAttribute(Attr);
			if (!B) { return Default; }
			if (B->GetTypeId() == PCG::Private::MetadataTypes<double>::Id) { return static_cast<const FPCGMetadataAttribute<double>*>(B)->GetValueFromItemKey(Entry(i)); }
			if (B->GetTypeId() == PCG::Private::MetadataTypes<float>::Id) { return static_cast<const FPCGMetadataAttribute<float>*>(B)->GetValueFromItemKey(Entry(i)); }
			if (B->GetTypeId() == PCG::Private::MetadataTypes<int32>::Id) { return static_cast<const FPCGMetadataAttribute<int32>*>(B)->GetValueFromItemKey(Entry(i)); }
			if (B->GetTypeId() == PCG::Private::MetadataTypes<int64>::Id) { return static_cast<double>(static_cast<const FPCGMetadataAttribute<int64>*>(B)->GetValueFromItemKey(Entry(i))); }
			return Default;
		}
	};
}

bool FPCGRammsManifestElement::ExecuteInternal(FPCGContext* Context) const
{
	FRammsWorldManifest M;

	// Routes: a segment point is an origin + yaw with bounds (0..Length, y0..y1); the centreline is at mid-y.
	double BestScore = -1.0;
	for (const FPCGTaggedData& Tagged : Context->InputData.GetInputsByPin(RammsPCG::PinRoutes))
	{
		FReader R(Cast<UPCGBasePointData>(Tagged.Data));
		for (int32 i = 0; i < R.Num; ++i)
		{
			const FTransform& Xf = R.T(i);
			const double Length = R.GetD(i, RammsPCG::Length, (R.Max(i) - R.Min(i)).X);
			const double MidY = (R.Min(i).Y + R.Max(i).Y) * 0.5;
			FRammsManifestRoute Route;
			Route.Name = R.Get<FName>(i, RammsPCG::RouteName, NAME_None);
			Route.NominalWidth = R.GetD(i, RammsPCG::Width, R.Max(i).Y - R.Min(i).Y);
			Route.MinClearWidth = R.GetD(i, RammsPCG::ClearWidth, Route.NominalWidth);
			Route.bCompliant = R.Get<bool>(i, RammsPCG::bCompliant, true);
			const int32 Steps = FMath::Clamp(FMath::CeilToInt(Length / 500.0), 1, 64);
			for (int32 s = 0; s <= Steps; ++s)
			{
				Route.Points.Add(Xf.TransformPosition(FVector(Length * s / Steps, MidY, 0.0)));
			}
			// A polyline arrives as consecutive segments sharing a name (SegmentIndex > 0 continues the last one).
			const int32 Segment = static_cast<int32>(R.GetD(i, RammsPCG::SegmentIndex, 0.0));
			if (Segment > 0 && M.Routes.Num() > 0 && M.Routes.Last().Name == Route.Name)
			{
				FRammsManifestRoute& Prev = M.Routes.Last();
				for (int32 k = 1; k < Route.Points.Num(); ++k) { Prev.Points.Add(Route.Points[k]); }
				Prev.MinClearWidth = FMath::Min(Prev.MinClearWidth, Route.MinClearWidth);
				Prev.bCompliant = Prev.bCompliant && Route.bCompliant;
				continue;
			}
			M.Routes.Add(Route);
			// Start on the widest compliant route, a little way in, facing along it.
			const double Score = (Route.bCompliant ? 1000.0 : 0.0) + Route.MinClearWidth + FMath::Min(Length, 3000.0) * 0.01;
			if (Score > BestScore && Length > 250.0)
			{
				BestScore = Score;
				M.SuggestedStart = FTransform(Xf.GetRotation(), Xf.TransformPosition(FVector(FMath::Min(150.0, Length * 0.25), MidY, 8.0)));
			}
		}
	}

	for (const FPCGTaggedData& Tagged : Context->InputData.GetInputsByPin(RammsPCG::PinFeatures))
	{
		FReader R(Cast<UPCGBasePointData>(Tagged.Data));
		for (int32 i = 0; i < R.Num; ++i)
		{
			const FName Kind = R.Get<FName>(i, RammsPCG::ManifestKind, NAME_None);
			if (Kind.IsNone()) { continue; }
			FRammsManifestFeature F;
			F.Kind = Kind;
			F.Subtype = R.Get<FName>(i, RammsPCG::ManifestSubtype, NAME_None);
			F.Location = R.T(i).GetLocation();
			F.Rotation = R.T(i).Rotator();
			F.Size = (R.Max(i) - R.Min(i)) * R.T(i).GetScale3D();
			F.bCompliant = R.Get<bool>(i, RammsPCG::bCompliant, true);
			F.Issue = static_cast<ERammsComplianceIssue>(FMath::Clamp(static_cast<int32>(R.GetD(i, RammsPCG::IssueId, 0.0)), 0, static_cast<int32>(ERammsComplianceIssue::ObstacleInRoute)));
			F.Measure = R.GetD(i, RammsPCG::Measure, 0.0);
			F.Threshold = R.GetD(i, RammsPCG::Threshold, 0.0);
			M.Features.Add(F);
		}
	}

	FString Json;
	FJsonObjectConverter::UStructToJsonObjectString(M, Json, 0, 0, 0, nullptr, /*bPrettyPrint=*/false);
	RammsPCG::EmitStringParam(Context, RammsPCG::PinOut, RammsPCG::Json, Json);
	return true;
}

#undef LOCTEXT_NAMESPACE
