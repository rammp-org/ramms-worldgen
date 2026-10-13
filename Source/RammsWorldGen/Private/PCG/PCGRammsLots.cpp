// Copyright ATDev. All Rights Reserved.
#include "PCG/PCGRammsLots.h"

#include "PCGContext.h"
#include "PCGPin.h"
#include "Data/PCGBasePointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"

#define LOCTEXT_NAMESPACE "RammsPCG"

namespace
{
	// Read helpers over an input point data. Missing attributes give defaults so
	// a graph can feed these nodes from hand-made points too.
	struct FPointReader
	{
		const UPCGBasePointData* Data = nullptr;
		int32 Num = 0;

		explicit FPointReader(const UPCGBasePointData* In) : Data(In)
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
		FVector Size(int32 i) const { return Max(i) - Min(i); }
	};
}

// --- Subdivide Lots ----------------------------------------------------------------

TArray<FPCGPinProperties> UPCGRammsSubdivideLotsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace_GetRef(RammsPCG::PinIn, EPCGDataType::Point, true, true, LOCTEXT("LotsIn", "Blocks from Street Network")).SetRequiredPin();
	return Pins;
}

TArray<FPCGPinProperties> UPCGRammsSubdivideLotsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(RammsPCG::PinLots, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinParks, EPCGDataType::Point);
	return Pins;
}

FPCGElementPtr UPCGRammsSubdivideLotsSettings::CreateElement() const
{
	return MakeShared<FPCGRammsSubdivideLotsElement>();
}

bool FPCGRammsSubdivideLotsElement::ExecuteInternal(FPCGContext* Context) const
{
	const UPCGRammsSubdivideLotsSettings* S = Context->GetInputSettings<UPCGRammsSubdivideLotsSettings>();
	check(S);
	const int32 Seed = Context->GetSeed();
	FRammsPointWriter Lots, Parks;
	int32 LotId = 0;

	for (const FPCGTaggedData& Tagged : Context->InputData.GetInputsByPin(RammsPCG::PinIn))
	{
		FPointReader R(Cast<UPCGBasePointData>(Tagged.Data));
		for (int32 i = 0; i < R.Num; ++i)
		{
			const int32 BlockId = static_cast<int32>(R.GetD(i, RammsPCG::BlockId, i));
			const bool bPark = R.Get<bool>(i, RammsPCG::IsPark, false);
			const FVector Centre = R.T(i).GetLocation();
			const FVector Size = R.Size(i);
			if (bPark || Size.X < S->LotWidthMin || Size.Y < 300.0)
			{
				const int32 p = Parks.Add(R.T(i), R.Min(i), R.Max(i), RammsPCG::SubSeed(Seed, 500 + BlockId));
				Parks.SetInt(p, RammsPCG::BlockId, BlockId);
				Parks.Set(p, RammsPCG::Width, Size.X);
				Parks.Set(p, RammsPCG::Depth, Size.Y);
				Parks.SetBool(p, RammsPCG::IsPark, true);
				continue;
			}
			FRandomStream Rng(RammsPCG::SubSeed(Seed, 700 + BlockId));
			const bool bTwoRows = Size.Y > 2.0 * S->MinLotDepth;
			const int32 Rows = bTwoRows ? 2 : 1;
			const double RowDepth = Size.Y / Rows;
			for (int32 Row = 0; Row < Rows; ++Row)
			{
				// Row 0 faces south (-Y), row 1 faces north (+Y).
				const double FaceSign = (Row == 0) ? -1.0 : 1.0;
				const double Yaw = (Row == 0) ? -90.0 : 90.0; // +X -> -Y, or +X -> +Y
				const double RowY = Centre.Y - Size.Y * 0.5 + RowDepth * (Row + 0.5);
				double x = Centre.X - Size.X * 0.5;
				const double xEnd = Centre.X + Size.X * 0.5;
				while (xEnd - x > S->LotWidthMin * 0.5)
				{
					double W = RammsPCG::RandRange(Rng, S->LotWidthMin, S->LotWidthMax);
					if (xEnd - (x + W) < S->LotWidthMin * 0.6) { W = xEnd - x; }
					const FVector LotCentre(x + W * 0.5, RowY, Centre.Z);
					// Local frame: +X toward the street (depth axis), Y along the frontage (width).
					const int32 l = Lots.Add(FTransform(FRotator(0.0, Yaw, 0.0), LotCentre), FVector(-RowDepth * 0.5, -W * 0.5, 0.0), FVector(RowDepth * 0.5, W * 0.5, 0.0), RammsPCG::SubSeed(Seed, 900 + LotId));
					Lots.SetInt(l, RammsPCG::LotId, LotId++);
					Lots.SetInt(l, RammsPCG::BlockId, BlockId);
					Lots.Set(l, RammsPCG::Width, W);
					Lots.Set(l, RammsPCG::Depth, RowDepth);
					Lots.Set(l, RammsPCG::Setback, RammsPCG::RandRange(Rng, S->SetbackMin, S->SetbackMax));
					Lots.SetInt(l, RammsPCG::Side, Row == 0 ? 0 : 2);
					(void)FaceSign;
					x += W;
				}
			}
		}
	}
	Lots.Emit(Context, RammsPCG::PinLots);
	Parks.Emit(Context, RammsPCG::PinParks);
	return true;
}

// --- Building Massing ------------------------------------------------------------------

TArray<FPCGPinProperties> UPCGRammsBuildingMassingSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace_GetRef(RammsPCG::PinIn, EPCGDataType::Point, true, true, LOCTEXT("MassingIn", "Lots from Subdivide Lots")).SetRequiredPin();
	return Pins;
}

TArray<FPCGPinProperties> UPCGRammsBuildingMassingSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(RammsPCG::PinPrimitives, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinPieces, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinProps, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinFeatures, EPCGDataType::Point);
	return Pins;
}

FPCGElementPtr UPCGRammsBuildingMassingSettings::CreateElement() const
{
	return MakeShared<FPCGRammsBuildingMassingElement>();
}

bool FPCGRammsBuildingMassingElement::ExecuteInternal(FPCGContext* Context) const
{
	const UPCGRammsBuildingMassingSettings* S = Context->GetInputSettings<UPCGRammsBuildingMassingSettings>();
	check(S);
	const int32 Seed = Context->GetSeed();
	const URammsWorldPalette* Palette = RammsPCG::LoadPalette(S->Palette, Context);
	const FString MatFacade = RammsPCG::Mat(Palette, RammsPaletteSlots::MatFacade);
	const FString MatFacadeAlt = RammsPCG::Mat(Palette, RammsPaletteSlots::MatFacadeAlt);
	const FString MatGlass = RammsPCG::Mat(Palette, RammsPaletteSlots::MatGlass);
	const FString MatConcrete = RammsPCG::Mat(Palette, RammsPaletteSlots::MatConcrete);
	const FString MatDoor = RammsPCG::Mat(Palette, RammsPaletteSlots::MatDoor);
	const FString MatMetal = RammsPCG::Mat(Palette, RammsPaletteSlots::MatMetal);
	const FString MatTrim = RammsPCG::Mat(Palette, RammsPaletteSlots::MatTrim);
	const FString MatRoof = RammsPCG::Mat(Palette, RammsPaletteSlots::MatRoof);
	const FString MatStorefront = RammsPCG::Mat(Palette, RammsPaletteSlots::MatStorefront);

	FRammsPointWriter Primitives, Pieces, Props, Features;
	int32 Counter = 0;
	auto NextSeed = [&]() { return RammsPCG::SubSeed(Seed, 2000 + (++Counter)); };

	for (const FPCGTaggedData& Tagged : Context->InputData.GetInputsByPin(RammsPCG::PinIn))
	{
		FPointReader R(Cast<UPCGBasePointData>(Tagged.Data));
		for (int32 i = 0; i < R.Num; ++i)
		{
			const int32 LotId = static_cast<int32>(R.GetD(i, RammsPCG::LotId, i));
			FRandomStream Rng(RammsPCG::SubSeed(Seed, 3000 + LotId));
			const FTransform& Xf = R.T(i);
			const FVector Size = R.Size(i);           // X = depth (toward street), Y = width
			const double Depth = Size.X, Width = Size.Y;
			const double Setback = FMath::Clamp(R.GetD(i, RammsPCG::Setback, 0.0), 0.0, Depth * 0.6);
			const double Z0 = Xf.GetLocation().Z;
			const double Yaw = Xf.Rotator().Yaw;
			auto ToWorld = [&](double lx, double ly, double lz) { return Xf.TransformPosition(FVector(lx, ly, lz - Z0)); };

			const double BW = FMath::Clamp(Width * FMath::Clamp(S->LotCoverage, 0.2, 1.0), 300.0, Width);
			const double FrontX = Depth * 0.5 - Setback;
			// Buildings sit at the front of the lot and are never much deeper than they are wide, so the
			// windowed facade is the long face on the street; the back of a deep lot stays open.
			const double BackX = FMath::Max(-Depth * 0.5 + Depth * 0.08, FrontX - FMath::Max(BW * 1.3, 600.0));
			const double BD = FMath::Max(FrontX - BackX, 300.0);
			const int32 Floors = FMath::Max(1, Rng.RandRange(FMath::Min(S->FloorsMin, S->FloorsMax), FMath::Max(S->FloorsMin, S->FloorsMax)));
			const double H = Floors * FMath::Max(S->FloorHeight, 250.0);

			// Shell, roof and the dressing that keeps it from reading as a box: a storefront band on the
			// ground floor of taller buildings, a trim band at every floor line, a cornice and a parapet.
			const double FH = FMath::Max(S->FloorHeight, 250.0);
			const FVector ShellCentre = ToWorld((FrontX + BackX) * 0.5, 0.0, Z0 + H * 0.5);
			RammsPCG::WriteCube(Primitives, ShellCentre, FVector(BD, BW, H), Yaw, Rng.FRand() < 0.5 ? MatFacade : MatFacadeAlt, NextSeed());
			const bool bHouse = S->FloorsMax <= 2 && Setback > 100.0; // set-back low-rise = a house: pitched roof, no parapet
			if (bHouse)
			{
				FRammsPieceDesc Rf;
				Rf.Kind = ERammsProcPieceKind::Roof;
				const bool bRidgeAlongWidth = BW >= BD;
				Rf.SizeX = (bRidgeAlongWidth ? BD : BW) + 50.0;   // span across the ridge, with eaves
				Rf.SizeY = (bRidgeAlongWidth ? BW : BD) + 30.0;   // ridge length
				Rf.SizeZ = Rf.SizeX * RammsPCG::RandRange(Rng, 0.22, 0.32);
				Rf.MaterialPath = MatRoof; Rf.MaterialPath2 = MatTrim;
				Rf.Transform = FTransform(FRotator(0.0, Yaw + (bRidgeAlongWidth ? 0.0 : 90.0), 0.0), ToWorld((FrontX + BackX) * 0.5, 0.0, Z0 + H));
				Rf.Seed = NextSeed();
				RammsPCG::WritePiece(Pieces, Rf);
			}
			else
			{
				RammsPCG::WriteCube(Primitives, ToWorld((FrontX + BackX) * 0.5, 0.0, Z0 + H + 2.0), FVector(BD - 6.0, BW - 6.0, 4.0), Yaw, MatRoof, NextSeed());
			}
			const bool bStorefront = Floors >= 3 && Setback < 150.0 && Rng.FRand() < 0.6;
			const bool bTrim = !bHouse && Rng.FRand() < 0.75;
			if (bTrim)
			{
				for (int32 Floor = 1; Floor < Floors; ++Floor)
				{
					RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 4.0, 0.0, Z0 + Floor * FH), FVector(8.0, BW + 8.0, 10.0), Yaw, MatTrim, NextSeed());
				}
				// Cornice.
				RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 10.0, 0.0, Z0 + H - 12.0), FVector(20.0, BW + 20.0, 24.0), Yaw, MatTrim, NextSeed());
			}
			if (!bHouse && Floors >= 2 && Rng.FRand() < 0.7)
			{
				// Parapet around the roof edge.
				const double PT = 20.0, PH = RammsPCG::RandRange(Rng, 50.0, 90.0);
				const FString MatP = bTrim ? MatTrim : MatConcrete;
				RammsPCG::WriteCube(Primitives, ToWorld(FrontX - PT * 0.5, 0.0, Z0 + H + PH * 0.5), FVector(PT, BW, PH), Yaw, MatP, NextSeed());
				RammsPCG::WriteCube(Primitives, ToWorld(BackX + PT * 0.5, 0.0, Z0 + H + PH * 0.5), FVector(PT, BW, PH), Yaw, MatP, NextSeed());
				RammsPCG::WriteCube(Primitives, ToWorld((FrontX + BackX) * 0.5, BW * 0.5 - PT * 0.5, Z0 + H + PH * 0.5), FVector(BD, PT, PH), Yaw, MatP, NextSeed());
				RammsPCG::WriteCube(Primitives, ToWorld((FrontX + BackX) * 0.5, -BW * 0.5 + PT * 0.5, Z0 + H + PH * 0.5), FVector(BD, PT, PH), Yaw, MatP, NextSeed());
			}
			// Rooftop units on anything with a flat roof worth the name.
			if (!bHouse && BD > 500.0 && BW > 500.0)
			{
				const int32 NumUnits = Rng.RandRange(Floors >= 3 ? 1 : 0, FMath::Min(3, static_cast<int32>(BD * BW / 1.2e6)));
				for (int32 u = 0; u < NumUnits; ++u)
				{
					const FVector P = ToWorld(RammsPCG::RandRange(Rng, BackX + 150.0, FrontX - 150.0), RammsPCG::RandRange(Rng, -BW * 0.5 + 150.0, BW * 0.5 - 150.0), Z0 + H + 4.0);
					const int32 q = RammsPCG::WritePaletteMesh(Props, Palette, RammsPaletteSlots::RoofUnit, Rng, P, Yaw + Rng.RandRange(0, 3) * 90.0, NextSeed());
					if (q < 0)
					{
						RammsPCG::WriteCube(Primitives, P + FVector(0, 0, 60.0), FVector(RammsPCG::RandRange(Rng, 120.0, 220.0), RammsPCG::RandRange(Rng, 100.0, 180.0), 120.0), Yaw, MatMetal, NextSeed());
					}
				}
			}

			// Entrance.
			const double EntranceY = RammsPCG::RandRange(Rng, -BW * 0.5 + 200.0, BW * 0.5 - 200.0);
			double DoorW = S->DoorClearWidth + 10.0;
			bool bDoorNarrow = false;
			if (Rng.FRand() < S->NonCompliance * 0.5)
			{
				DoorW = RammsPCG::RandRange(Rng, 60.0, S->DoorClearWidth - 2.0);
				bDoorNarrow = true;
			}
			const bool bStepped = Rng.FRand() < FMath::Clamp(S->EntranceStepChance, 0.0, 1.0);
			int32 Steps = 0; double StepRise = 0.0;
			bool bRamp = false, bRampSteep = false;
			double RampSlope = 0.0;
			if (bStepped)
			{
				Steps = Rng.RandRange(1, 3);
				StepRise = RammsPCG::RandRange(Rng, 12.0, 17.0);
				bRamp = Rng.FRand() < FMath::Clamp(S->EntranceRampChance, 0.0, 1.0);
				bRampSteep = bRamp && Rng.FRand() < S->NonCompliance;
			}
			const double EntranceRise = Steps * StepRise;
			double Threshold = 0.0;
			if (!bStepped && Rng.FRand() < S->NonCompliance * 0.6)
			{
				Threshold = RammsPCG::RandRange(Rng, 2.0, 5.0);
			}

			// Door leaf flat on the facade; its frame runs along the facade (lot Y).
			{
				FRammsPieceDesc D;
				D.Kind = ERammsProcPieceKind::Door;
				D.SizeX = DoorW; D.SizeY = 5.0; D.SizeZ = 210.0; D.Rise = Threshold;
				D.MaterialPath = MatDoor; D.MaterialPath2 = MatMetal;
				D.ManifestKind = FName(TEXT("Door"));
				D.Transform = FTransform(FRotator(0.0, Yaw + 90.0, 0.0), ToWorld(FrontX + 3.0, EntranceY - DoorW * 0.5, Z0 + EntranceRise));
				D.Seed = NextSeed();
				if (bDoorNarrow) { D.bCompliant = false; D.Issue = ERammsComplianceIssue::DoorTooNarrow; D.ManifestSubtype = FName(TEXT("Narrow")); D.Measure = DoorW; D.Threshold = S->DoorClearWidth; }
				else if (Threshold > S->DoorThresholdMax) { D.bCompliant = false; D.Issue = ERammsComplianceIssue::ThresholdTooHigh; D.ManifestSubtype = FName(TEXT("RaisedThreshold")); D.Measure = Threshold; D.Threshold = S->DoorThresholdMax; }
				else { D.ManifestSubtype = FName(TEXT("Standard")); D.Measure = DoorW; D.Threshold = S->DoorClearWidth; }
				RammsPCG::WritePiece(Pieces, D);
			}
			const double StepsW = DoorW + 80.0;
			const double Tread = 30.0;
			if (bStepped)
			{
				FRammsPieceDesc St;
				St.Kind = ERammsProcPieceKind::Step;
				St.StepCount = Steps; St.Rise = StepRise; St.SizeX = Tread; St.SizeY = StepsW;
				St.MaterialPath = MatConcrete;
				St.ManifestKind = FName(TEXT("Step"));
				St.Measure = EntranceRise; St.Threshold = 0.64;
				// Steps rise toward their local -X; put the bottom on the street side and the top at the facade.
				St.Transform = FTransform(FRotator(0.0, Yaw + 180.0, 0.0), ToWorld(FrontX + Steps * Tread, EntranceY, Z0));
				St.bCompliant = bRamp && !bRampSteep; // steps are fine when a usable ramp exists beside them
				St.Issue = St.bCompliant ? ERammsComplianceIssue::None : ERammsComplianceIssue::StepAtEntrance;
				St.ManifestSubtype = bRamp ? FName(TEXT("WithRamp")) : FName(TEXT("NoRamp"));
				St.Seed = NextSeed();
				RammsPCG::WritePiece(Pieces, St);
				if (bRamp)
				{
					FRammsPieceDesc Rp;
					Rp.Kind = ERammsProcPieceKind::Ramp;
					Rp.Rise = EntranceRise;
					Rp.Slope = bRampSteep ? S->RampMaxSlope * RammsPCG::RandRange(Rng, 1.4, 2.2) : S->RampMaxSlope;
					RampSlope = Rp.Slope;
					Rp.SizeY = 120.0;
					Rp.LandingSize = S->LandingSize;
					Rp.MaterialPath = MatConcrete; Rp.MaterialPath2 = MatMetal;
					Rp.ManifestKind = FName(TEXT("Ramp"));
					Rp.Measure = Rp.Slope; Rp.Threshold = S->RampMaxSlope;
					Rp.bCompliant = !bRampSteep;
					Rp.Issue = bRampSteep ? ERammsComplianceIssue::SlopeTooSteep : ERammsComplianceIssue::None;
					Rp.ManifestSubtype = bRampSteep ? FName(TEXT("TooSteep")) : FName(TEXT("Standard"));
					const double Run = EntranceRise / Rp.Slope;
					// Beside the steps, same orientation: low end on the street side, landing at the facade.
					const double RampY = EntranceY + (StepsW * 0.5 + Rp.SizeY * 0.5 + 20.0) * (Rng.FRand() < 0.5 ? 1.0 : -1.0);
					Rp.Transform = FTransform(FRotator(0.0, Yaw + 180.0, 0.0), ToWorld(FrontX + Run + Rp.LandingSize, RampY, Z0));
					Rp.Seed = NextSeed();
					RammsPCG::WritePiece(Pieces, Rp);
				}
			}
			// Entrance summary for the manifest.
			{
				const int32 f = Features.AddBox(ToWorld(FrontX, EntranceY, Z0), Yaw, FVector(60.0, StepsW, 10.0), NextSeed());
				const bool bAccessible = !bDoorNarrow && Threshold <= S->DoorThresholdMax && (!bStepped || (bRamp && !bRampSteep));
				const FName Subtype = !bStepped ? FName(TEXT("Level")) : (bRamp ? (bRampSteep ? FName(TEXT("SteppedSteepRamp")) : FName(TEXT("SteppedWithRamp"))) : FName(TEXT("SteppedNoRamp")));
				// Measure / threshold describe the issue that was reported (a compliant entrance reports its rise).
				ERammsComplianceIssue Issue = ERammsComplianceIssue::None;
				double Measure = EntranceRise, Limit = 0.0;
				if (bDoorNarrow) { Issue = ERammsComplianceIssue::DoorTooNarrow; Measure = DoorW; Limit = S->DoorClearWidth; }
				else if (Threshold > S->DoorThresholdMax) { Issue = ERammsComplianceIssue::ThresholdTooHigh; Measure = Threshold; Limit = S->DoorThresholdMax; }
				else if (bStepped && !bRamp) { Issue = ERammsComplianceIssue::StepAtEntrance; Measure = EntranceRise; Limit = S->VerticalChangeMax; }
				else if (bRampSteep) { Issue = ERammsComplianceIssue::SlopeTooSteep; Measure = RampSlope; Limit = S->RampMaxSlope; }
				RammsPCG::WriteManifest(Features, f, FName(TEXT("Entrance")), Subtype, bAccessible, Issue, Measure, Limit);
				Features.SetInt(f, RammsPCG::LotId, LotId);
			}

			// Yard trees in the open ground beside or behind the building (procedural stand-ins,
			// swapped for the palette's tree mesh by the spawner when one is assigned).
			if (S->YardTreeChance > 0.0 && Rng.FRand() < FMath::Clamp(S->YardTreeChance, 0.0, 1.0))
			{
				const double SideRoom = (Width - BW) * 0.5;
				const double BackRoom = BackX - (-Depth * 0.5);
				double tx = 0.0, ty = 0.0;
				bool bPlaced = false;
				if (BackRoom > 500.0)
				{
					tx = RammsPCG::RandRange(Rng, -Depth * 0.5 + 200.0, BackX - 250.0);
					ty = RammsPCG::RandRange(Rng, -Width * 0.5 + 200.0, Width * 0.5 - 200.0);
					bPlaced = true;
				}
				else if (SideRoom > 350.0)
				{
					const double Side = Rng.FRand() < 0.5 ? -1.0 : 1.0;
					tx = RammsPCG::RandRange(Rng, BackX + 100.0, FrontX - 100.0);
					ty = Side * (BW * 0.5 + SideRoom * 0.5);
					bPlaced = true;
				}
				if (bPlaced)
				{
					FRammsPieceDesc T;
					T.Kind = ERammsProcPieceKind::Tree;
					T.SizeX = RammsPCG::RandRange(Rng, 350.0, 650.0);
					T.SizeZ = RammsPCG::RandRange(Rng, 550.0, 950.0);
					T.MaterialPath = RammsPCG::Mat(Palette, RammsPaletteSlots::MatFoliage);
					T.MaterialPath2 = RammsPCG::Mat(Palette, RammsPaletteSlots::MatTrunk);
					T.ManifestKind = FName(TEXT("Tree"));
					T.Transform = FTransform(FRotator(0.0, RammsPCG::RandRange(Rng, 0.0, 360.0), 0.0), ToWorld(tx, ty, Z0));
					T.Seed = NextSeed();
					RammsPCG::WritePiece(Pieces, T);
				}
			}

			// Entrance canopy and a dark door surround.
			RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 2.0, EntranceY, Z0 + EntranceRise + 110.0), FVector(4.0, DoorW + 30.0, 225.0), Yaw, MatStorefront, NextSeed());
			if (Rng.FRand() < 0.5)
			{
				RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 45.0, EntranceY, Z0 + EntranceRise + 235.0), FVector(90.0, DoorW + 90.0, 8.0), Yaw, MatStorefront, NextSeed());
			}

			// Ground floor storefront: glass between dark pilasters, a fascia above, no windows on that floor.
			if (bStorefront)
			{
				const double Sill = 35.0, Top = FH - 45.0;
				const double PilW = 20.0;
				double y = -BW * 0.5 + 10.0;
				while (y < BW * 0.5 - 10.0)
				{
					const double Bay = FMath::Min(RammsPCG::RandRange(Rng, 220.0, 360.0), BW * 0.5 - 10.0 - y);
					const double yc = y + Bay * 0.5;
					if (FMath::Abs(yc - EntranceY) > DoorW * 0.5 + Bay * 0.5 + 10.0 && Bay > 80.0)
					{
						RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 3.0, yc, Z0 + (Sill + Top) * 0.5), FVector(6.0, Bay - PilW, Top - Sill), Yaw, MatGlass, NextSeed());
						RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 5.0, yc, Z0 + Sill * 0.5), FVector(10.0, Bay - PilW, Sill), Yaw, MatStorefront, NextSeed());
					}
					if (FMath::Abs(y + Bay - EntranceY) > DoorW * 0.5 + 25.0 && y + Bay < BW * 0.5 - 10.0)
					{
						RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 6.0, y + Bay, Z0 + Top * 0.5), FVector(12.0, PilW, Top), Yaw, MatStorefront, NextSeed());
					}
					y += Bay;
				}
				RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 8.0, 0.0, Z0 + FH - 22.0), FVector(16.0, BW + 16.0, 44.0), Yaw, MatStorefront, NextSeed());
			}

			// Windows on the front facade: glass in a dark frame with a sill.
			if (S->WindowSpacing > 50.0)
			{
				const double WinW = FMath::Clamp(S->WindowSpacing * 0.45, 70.0, 140.0), WinH = FMath::Clamp(FH * 0.45, 100.0, 170.0);
				for (int32 Floor = bStorefront ? 1 : 0; Floor < Floors; ++Floor)
				{
					for (double y = -BW * 0.5 + S->WindowSpacing * 0.5; y < BW * 0.5 - 60.0; y += S->WindowSpacing)
					{
						if (Floor == 0 && FMath::Abs(y - EntranceY) < DoorW * 0.5 + 100.0) { continue; }
						const double zc = Z0 + Floor * FH + FH * 0.5 + 15.0;
						RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 2.0, y, zc), FVector(4.0, WinW + 14.0, WinH + 14.0), Yaw, MatStorefront, NextSeed());
						RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 3.0, y, zc), FVector(6.0, WinW, WinH), Yaw, MatGlass, NextSeed());
						RammsPCG::WriteCube(Primitives, ToWorld(FrontX + 6.0, y, zc - WinH * 0.5 - 10.0), FVector(12.0, WinW + 20.0, 6.0), Yaw, MatTrim, NextSeed());
					}
				}
			}
		}
	}
	Primitives.Emit(Context, RammsPCG::PinPrimitives, { TEXT("Building") });
	Pieces.Emit(Context, RammsPCG::PinPieces, { TEXT("Building") });
	Props.Emit(Context, RammsPCG::PinProps, { TEXT("Building") });
	Features.Emit(Context, RammsPCG::PinFeatures);
	return true;
}

#undef LOCTEXT_NAMESPACE
