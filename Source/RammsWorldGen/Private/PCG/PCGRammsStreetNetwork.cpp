// Copyright ATDev. All Rights Reserved.
#include "PCG/PCGRammsStreetNetwork.h"

#include "PCG/RammsGround.h"
#include "PCGContext.h"
#include "RammsWorldGenModule.h"
#include "PCGModule.h"
#include "PCGPin.h"

#define LOCTEXT_NAMESPACE "RammsPCG"

TArray<FPCGPinProperties> UPCGRammsStreetNetworkSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	FPCGPinProperties& In = Pins.Emplace_GetRef(RammsPCG::PinIn, EPCGDataType::Spatial, /*bAllowMultipleConnections=*/false, /*bAllowMultipleData=*/false,
		LOCTEXT("InTooltip", "The generator's bounds. Wire the Input node's In pin here."));
	In.SetRequiredPin();
	return Pins;
}

TArray<FPCGPinProperties> UPCGRammsStreetNetworkSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(RammsPCG::PinPrimitives, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinProps, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinPieces, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinBlocks, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinSidewalks, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinFeatures, EPCGDataType::Point);
	Pins.Emplace(RammsPCG::PinRoads, EPCGDataType::Spline);
	return Pins;
}

FPCGElementPtr UPCGRammsStreetNetworkSettings::CreateElement() const
{
	return MakeShared<FPCGRammsStreetNetworkElement>();
}

namespace
{
	struct FSeg
	{
		bool bAlongX = true; // runs along X at y = Lines Y[j], between X[i] and X[i+1]; else along Y at x = X[i] between Y[j] and Y[j+1]
		int32 I = 0, J = 0;
		bool bLive = true;
	};

	struct FBlock
	{
		int32 Id = 0;
		double X0 = 0, X1 = 0, Y0 = 0, Y1 = 0; // centreline rectangle
		bool bPark = false;
		bool bLawn = false; // ground is lawn (parks always; built blocks when LawnLots says so)
	};

	// One side of a block's sidewalk ring. Local frame: origin at Start on the
	// curb line, +X along Dir, +Y = Inward (toward the lots). Yaw rotates world
	// +X onto Dir, and world +Y onto Inward.
	struct FSide
	{
		int32 BlockId = 0;
		int32 Index = 0; // 0 south, 1 east, 2 north, 3 west
		FVector Start = FVector::ZeroVector;
		FVector Dir = FVector::ForwardVector;
		FVector Inward = FVector::RightVector;
		double Length = 0.0;
		double Yaw = 0.0;
		TArray<FVector2D> Keepouts; // [along start, along end] of curb ramps and flares: the curb is open there
		TArray<FVector2D> Clear;    // [along start, along end] kept free of props and parked cars (every crossing)
		TArray<FVector2D> CornerCrossings; // [along start, along end] of each intersection crossing (not mid-block), its ramp and flares included
		double StripDepth = 0.0;    // planting strip width at the curb (0 = paved to the curb)
	};

	FVector2D RandomLineSpacings(const FRandomStream& Rng, double Total, double Min, double Max, double RoadWidth, TArray<double>& Out)
	{
		// Lines from -Total/2 + RW/2 to Total/2 - RW/2, with block sizes drawn from [Min, Max].
		Out.Reset();
		const double Lo = -Total * 0.5 + RoadWidth * 0.5;
		const double Hi = Total * 0.5 - RoadWidth * 0.5;
		Out.Add(Lo);
		double Pos = Lo;
		while (true)
		{
			const double Block = RammsPCG::RandRange(Rng, Min, Max);
			const double Next = Pos + Block + RoadWidth;
			if (Next + (Min + RoadWidth) * 0.6 > Hi)
			{
				break;
			}
			Out.Add(Next);
			Pos = Next;
		}
		if (Out.Last() < Hi - 1.0)
		{
			Out.Add(Hi);
		}
		return FVector2D(Lo, Hi);
	}
}

bool FPCGRammsStreetNetworkElement::ExecuteInternal(FPCGContext* Context) const
{
	const UPCGRammsStreetNetworkSettings* S = Context->GetInputSettings<UPCGRammsStreetNetworkSettings>();
	check(S);
	{
		static int32 ExecCount = 0;
		++ExecCount;
		const int32 NumIn = Context->InputData.GetInputsByPin(RammsPCG::PinIn).Num();
		const int32 NumOverrides = Context->InputData.GetParamsByPin(FName(TEXT("Overrides"))).Num();
		const int32 NumAll = Context->InputData.TaggedData.Num();
		UE_LOG(LogRammsWorldGen, Verbose, TEXT("%s: execution #%d, In=%d Overrides=%d all inputs=%d seed=%d"), *S->GetName(), ExecCount, NumIn, NumOverrides, NumAll, Context->GetSeed());
		for (const FPCGTaggedData& T : Context->InputData.TaggedData)
		{
			UE_LOG(LogRammsWorldGen, Verbose, TEXT("    input pin=%s data=%s tags=%s"), *T.Pin.ToString(), T.Data ? *T.Data->GetClass()->GetName() : TEXT("null"), *FString::Join(T.Tags.Array(), TEXT(",")));
		}
	}

	const int32 Seed = Context->GetSeed();
	FRandomStream Rng(Seed);
	const URammsWorldPalette* Palette = RammsPCG::LoadPalette(S->Palette, Context);

	const FBox Bounds = RammsPCG::InputBounds(Context, RammsPCG::PinIn, FVector(S->SiteSizeX, S->SiteSizeY, 1000.0));
	const FVector Centre = Bounds.GetCenter();
	const double Z0 = Centre.Z;             // sidewalk level
	const double RW = FMath::Max(S->RoadWidth, 200.0);
	const double SW = FMath::Max(S->SidewalkWidth, 60.0);
	const double FZ = FMath::Clamp(S->FurnitureZoneWidth, 0.0, SW * 0.6);
	const double CH = FMath::Max(S->CurbHeight, 0.0);
	const double Zr = Z0 - CH;              // road level
	const double CW = FMath::Max(S->CrosswalkWidth, 120.0);
	const double SizeX = FMath::Max(Bounds.GetSize().X, 2.0 * (S->BlockSizeMin + RW));
	const double SizeY = FMath::Max(Bounds.GetSize().Y, 2.0 * (S->BlockSizeMin + RW));

	// --- lines ------------------------------------------------------------------------------
	TArray<double> X, Y;
	RandomLineSpacings(Rng, SizeX, S->BlockSizeMin, S->BlockSizeMax, RW, X);
	RandomLineSpacings(Rng, SizeY, S->BlockSizeMin, S->BlockSizeMax, RW, Y);
	// Jitter interior lines; keep order with a margin so blocks never collapse.
	const double JitterAmp = FMath::Clamp(S->StreetJitter, 0.0, 1.0) * 0.25 * S->BlockSizeMin;
	for (int32 i = 1; i + 1 < X.Num(); ++i) { X[i] = FMath::Clamp(X[i] + RammsPCG::RandRange(Rng, -JitterAmp, JitterAmp), X[i - 1] + RW + 2.0 * SW + 300.0, X[i + 1] - RW - 2.0 * SW - 300.0); }
	for (int32 j = 1; j + 1 < Y.Num(); ++j) { Y[j] = FMath::Clamp(Y[j] + RammsPCG::RandRange(Rng, -JitterAmp, JitterAmp), Y[j - 1] + RW + 2.0 * SW + 300.0, Y[j + 1] - RW - 2.0 * SW - 300.0); }
	for (double& V : X) { V += Centre.X; }
	for (double& V : Y) { V += Centre.Y; }
	const int32 NX = X.Num(), NY = Y.Num();
	if (NX < 2 || NY < 2)
	{
		PCGE_LOG(Warning, GraphAndLog, LOCTEXT("TooSmall", "Site too small for one block; nothing generated."));
		return true;
	}

	// --- segments and blocks ------------------------------------------------------------------
	TArray<FSeg> Segs;
	auto SegIndexX = [&](int32 i, int32 j) { return j * (NX - 1) + i; };               // X segments: i in [0,NX-2], j in [0,NY-1]
	auto SegIndexY = [&](int32 i, int32 j) { return (NX - 1) * NY + i * (NY - 1) + j; }; // Y segments: i in [0,NX-1], j in [0,NY-2]
	for (int32 j = 0; j < NY; ++j) for (int32 i = 0; i + 1 < NX; ++i) { FSeg& Sg = Segs.AddDefaulted_GetRef(); Sg.bAlongX = true; Sg.I = i; Sg.J = j; }
	for (int32 i = 0; i < NX; ++i) for (int32 j = 0; j + 1 < NY; ++j) { FSeg& Sg = Segs.AddDefaulted_GetRef(); Sg.bAlongX = false; Sg.I = i; Sg.J = j; }

	// Cells, then pairwise merges that drop the shared interior segment.
	TArray<int32> CellBlock; // cell (i,j) -> block index
	CellBlock.Init(-1, (NX - 1) * (NY - 1));
	auto Cell = [&](int32 i, int32 j) -> int32& { return CellBlock[j * (NX - 1) + i]; };
	TArray<FBlock> Blocks;
	const double Dropout = FMath::Clamp(S->StreetDropout, 0.0, 0.6);
	for (int32 j = 0; j + 1 < NY; ++j)
	{
		for (int32 i = 0; i + 1 < NX; ++i)
		{
			if (Cell(i, j) >= 0) { continue; }
			FBlock B; B.Id = Blocks.Num(); B.X0 = X[i]; B.X1 = X[i + 1]; B.Y0 = Y[j]; B.Y1 = Y[j + 1];
			Cell(i, j) = B.Id;
			if (Dropout > 0.0 && Rng.FRand() < Dropout)
			{
				// Merge with the unmerged neighbour to the east, else to the north.
				if (i + 2 < NX && Cell(i + 1, j) < 0)
				{
					Cell(i + 1, j) = B.Id; B.X1 = X[i + 2];
					Segs[SegIndexY(i + 1, j)].bLive = false;
				}
				else if (j + 2 < NY && Cell(i, j + 1) < 0)
				{
					Cell(i, j + 1) = B.Id; B.Y1 = Y[j + 2];
					Segs[SegIndexX(i, j + 1)].bLive = false;
				}
			}
			B.bPark = Rng.FRand() < FMath::Clamp(S->ParkFraction, 0.0, 1.0);
			B.bLawn = B.bPark || Rng.FRand() < FMath::Clamp(S->LawnLots, 0.0, 1.0);
			Blocks.Add(B);
		}
	}
	// A preset that asks for parks gets at least one when there is room for it, so a small site
	// with an unlucky seed still shows the park features it was built to show.
	if (S->ParkFraction > 0.0 && Blocks.Num() >= 3 && !Blocks.ContainsByPredicate([](const FBlock& B) { return B.bPark; }))
	{
		FBlock& B = Blocks[Rng.RandRange(0, Blocks.Num() - 1)];
		B.bPark = true; B.bLawn = true;
	}

	// --- ground: street grades, crown, cross slopes, curb heights, roughness -------------------------------------
	// With every terrain and roughness parameter at 0 none of this runs and the world is placed exactly as before. With
	// any of them on, the ground is a model every node evaluates (RammsGround.h), drawn from a random stream of its own
	// so the layout (blocks, lots, crossings, props) stays the one the flat world has for the seed, and the ground
	// surfaces are GroundPrisms pieces (exact convex prisms) instead of scaled cubes.
	const bool bRough = S->SurfaceRoughness > 0.0 || S->HeaveDensity > 0.0 || S->CrackDensity > 0.0 || S->CoverDensity > 0.0
		|| S->PavingPatchDensity > 0.0 || S->PotholeDensity > 0.0 || S->SpeedHumpChance > 0.0;
	const bool bGround = bRough || S->MaxStreetGrade > 0.0 || S->RoadCrown > 0.0 || S->SidewalkCrossSlope > 0.0
		|| S->NonCompliantCrossSlopeChance > 0.0 || S->CurbHeightMax > 0.0;
	FRammsGroundModel Gm;
	FString GroundText;
	FRandomStream GRng(RammsPCG::SubSeed(Seed, 77001));
	int32 GroundSeed = 0;
	auto NextGSeed = [&]() { return RammsPCG::SubSeed(Seed, 78000 + (++GroundSeed)); };
	if (bGround)
	{
		Gm.Z0 = Z0;
		Gm.RoadWidth = RW;
		Gm.SidewalkWidth = SW;
		Gm.CurbHeight = CH;
		Gm.Crown = FMath::Clamp(S->RoadCrown, 0.0, 0.08);
		Gm.X = X;
		Gm.Y = Y;
		// Each interval of the grid climbs at its own grade; the lowest line sits at the generator's height.
		const double MaxGrade = FMath::Clamp(S->MaxStreetGrade, 0.0, 0.15);
		auto Walk = [&](const TArray<double>& L, TArray<double>& F)
		{
			F.SetNum(L.Num());
			F[0] = 0.0;
			for (int32 k = 1; k < L.Num(); ++k) { F[k] = F[k - 1] + RammsPCG::RandRange(GRng, -MaxGrade, MaxGrade) * (L[k] - L[k - 1]); }
			double Lo = F[0];
			for (const double V : F) { Lo = FMath::Min(Lo, V); }
			for (double& V : F) { V -= Lo; }
		};
		Walk(X, Gm.FX);
		Walk(Y, Gm.FY);
		// Live road arms at every node, for the intersections' crowns.
		Gm.Arms.SetNumZeroed(NX * NY);
		for (int32 j = 0; j < NY; ++j)
		{
			for (int32 i = 0; i < NX; ++i)
			{
				uint8 A = 0;
				if (i + 1 < NX && Segs[SegIndexX(i, j)].bLive) { A |= static_cast<uint8>(FRammsGroundModel::ArmE); }
				if (i > 0 && Segs[SegIndexX(i - 1, j)].bLive) { A |= static_cast<uint8>(FRammsGroundModel::ArmW); }
				if (j + 1 < NY && Segs[SegIndexY(i, j)].bLive) { A |= static_cast<uint8>(FRammsGroundModel::ArmN); }
				if (j > 0 && Segs[SegIndexY(i, j - 1)].bLive) { A |= static_cast<uint8>(FRammsGroundModel::ArmS); }
				Gm.Arms[j * NX + i] = A;
			}
		}
		// Per block: its curb heights (a plane: every corner within [CurbHeightMin, CurbHeightMax], linear along each
		// side) and its sidewalks' cross slope (now and then steeper than MaxCrossSlope).
		const bool bVary = S->CurbHeightMax > 0.0;
		const double RLo = bVary ? FMath::Max(FMath::Min(S->CurbHeightMin, S->CurbHeightMax), 0.0) : CH;
		const double RHi = bVary ? FMath::Max(S->CurbHeightMax, RLo) : CH;
		for (const FBlock& B : Blocks)
		{
			FRammsGroundModel::FBlock& G = Gm.Blocks.AddDefaulted_GetRef();
			G.X0 = B.X0; G.X1 = B.X1; G.Y0 = B.Y0; G.Y1 = B.Y1;
			G.Reveal = CH;
			if (bVary)
			{
				const double M = RammsPCG::RandRange(GRng, RLo, RHi);
				const double Room = FMath::Min(M - RLo, RHi - M);
				const double Ax = RammsPCG::RandRange(GRng, -1.0, 1.0) * Room;
				const double Ay = RammsPCG::RandRange(GRng, -1.0, 1.0) * (Room - FMath::Abs(Ax));
				G.Reveal = M;
				G.RevealX = Ax / FMath::Max((B.X1 - B.X0 - RW) * 0.5, 1.0);
				G.RevealY = Ay / FMath::Max((B.Y1 - B.Y0 - RW) * 0.5, 1.0);
			}
			const bool bSteep = S->NonCompliantCrossSlopeChance > 0.0 && GRng.FRand() < S->NonCompliantCrossSlopeChance;
			if (bSteep)
			{
				G.CrossSlope = RammsPCG::RandRange(GRng, FMath::Max(S->MaxCrossSlope + 0.01, 0.03), FMath::Max(S->MaxCrossSlope * 3.0, 0.06));
			}
			else if (S->SidewalkCrossSlope > 0.0)
			{
				G.CrossSlope = FMath::Clamp(S->SidewalkCrossSlope, 0.0, 0.08) * RammsPCG::RandRange(GRng, 0.6, 1.0);
			}
		}
		// Every node evaluates the text: round-trip it so this one does too.
		GroundText = Gm.ToString();
		if (!Gm.FromString(GroundText))
		{
			UE_LOG(LogRammsWorldGen, Warning, TEXT("%s: the ground model did not round-trip"), *S->GetName());
		}
	}
	// A side-local rectangle (along, inward) as world polygons, each inside one face of the ground: split on the
	// block's corner diagonals where it reaches into a corner square, and on the streets dropped inside the block.
	struct FSideRef { int32 BlockId; FVector Start, Dir, Inward; };
	auto SidePolys = [&](const FSideRef& Sd, const FBox2D& R) -> TArray<TArray<FVector2D>>
	{
		auto Wp = [&](double A, double B) { const FVector P = Sd.Start + Sd.Dir * A + Sd.Inward * B; return FVector2D(P.X, P.Y); };
		TArray<TArray<FVector2D>> Out;
		Out.Add({ Wp(R.Min.X, R.Min.Y), Wp(R.Max.X, R.Min.Y), Wp(R.Max.X, R.Max.Y), Wp(R.Min.X, R.Max.Y) });
		FBox2D Bb(ForceInit);
		for (const FVector2D& V : Out[0]) { Bb += V; }
		const FBox2D Curb = Gm.CurbRect(Sd.BlockId);
		if (Curb.bIsValid)
		{
			const FVector2D Corners[4] = { Curb.Min, FVector2D(Curb.Max.X, Curb.Min.Y), Curb.Max, FVector2D(Curb.Min.X, Curb.Max.Y) };
			const FVector2D Signs[4] = { FVector2D(1.0, 1.0), FVector2D(-1.0, 1.0), FVector2D(-1.0, -1.0), FVector2D(1.0, -1.0) };
			for (int32 c = 0; c < 4; ++c)
			{
				const FVector2D Far = Corners[c] + Signs[c] * SW;
				const FBox2D Sq(FVector2D(FMath::Min(Corners[c].X, Far.X), FMath::Min(Corners[c].Y, Far.Y)), FVector2D(FMath::Max(Corners[c].X, Far.X), FMath::Max(Corners[c].Y, Far.Y)));
				if (!RammsPCG::Overlaps(Bb, Sq)) { continue; }
				const FVector2D N(Signs[c].Y, -Signs[c].X);
				RammsGround::SplitAllByLine(Out, N, FVector2D::DotProduct(N, Corners[c]));
			}
		}
		TArray<double> Ix, Iy;
		Gm.InnerLines(Sd.BlockId, Ix, Iy);
		for (const double V : Ix) { RammsGround::SplitAllByLine(Out, FVector2D(1.0, 0.0), V); }
		for (const double V : Iy) { RammsGround::SplitAllByLine(Out, FVector2D(0.0, 1.0), V); }
		return Out;
	};
	auto RefOf = [](const FSide& Sd) { return FSideRef{ Sd.BlockId, Sd.Start, Sd.Dir, Sd.Inward }; };
	const FName SurfAsphalt(TEXT("Asphalt")), SurfConcrete(TEXT("Concrete")), SurfGrass(TEXT("Grass")), SurfMetal(TEXT("Metal")), SurfBrick(TEXT("Brick")), SurfCobble(TEXT("Cobble"));

	// --- sides -----------------------------------------------------------------------------------
	TArray<FSide> Sides;
	for (const FBlock& B : Blocks)
	{
		const double ox0 = B.X0 + RW * 0.5, ox1 = B.X1 - RW * 0.5, oy0 = B.Y0 + RW * 0.5, oy1 = B.Y1 - RW * 0.5;
		auto AddSide = [&](int32 Index, const FVector& Start, const FVector& Dir, const FVector& Inward, double Length, double Yaw)
		{
			FSide& Sd = Sides.AddDefaulted_GetRef();
			Sd.BlockId = B.Id; Sd.Index = Index; Sd.Start = Start; Sd.Dir = Dir; Sd.Inward = Inward; Sd.Length = Length; Sd.Yaw = Yaw;
		};
		AddSide(0, FVector(ox0, oy0, Z0), FVector(1, 0, 0), FVector(0, 1, 0), ox1 - ox0, 0.0);
		AddSide(1, FVector(ox1, oy0, Z0), FVector(0, 1, 0), FVector(-1, 0, 0), oy1 - oy0, 90.0);
		AddSide(2, FVector(ox1, oy1, Z0), FVector(-1, 0, 0), FVector(0, -1, 0), ox1 - ox0, 180.0);
		AddSide(3, FVector(ox0, oy1, Z0), FVector(0, -1, 0), FVector(1, 0, 0), oy1 - oy0, 270.0);
	}
	for (FSide& Sd : Sides)
	{
		Sd.StripDepth = (S->PlantingStrip > 0.5 && FZ > 30.0 && !Blocks[Sd.BlockId].bPark) ? FZ : 0.0;
	}
	// Find the side of a block that faces a road line at a coordinate.
	auto FindSide = [&](bool bRoadAlongX, double Across, double Pos, bool bNegativeSide) -> FSide*
	{
		for (FSide& Sd : Sides)
		{
			const FBlock& B = Blocks[Sd.BlockId];
			if (bRoadAlongX)
			{
				// Road at y = Across. Negative side = block below (its north side, index 2); else block above (south, 0).
				const bool bMatch = bNegativeSide ? (Sd.Index == 2 && FMath::IsNearlyEqual(B.Y1, Across, 1.0)) : (Sd.Index == 0 && FMath::IsNearlyEqual(B.Y0, Across, 1.0));
				if (bMatch && Pos > B.X0 + RW * 0.5 && Pos < B.X1 - RW * 0.5) { return &Sd; }
			}
			else
			{
				// Road at x = Across. Negative side = block to the left (east side, 1); else block to the right (west, 3).
				const bool bMatch = bNegativeSide ? (Sd.Index == 1 && FMath::IsNearlyEqual(B.X1, Across, 1.0)) : (Sd.Index == 3 && FMath::IsNearlyEqual(B.X0, Across, 1.0));
				if (bMatch && Pos > B.Y0 + RW * 0.5 && Pos < B.Y1 - RW * 0.5) { return &Sd; }
			}
		}
		return nullptr;
	};
	auto AlongOnSide = [&](const FSide& Sd, const FVector& WorldPoint) -> double
	{
		return FVector::DotProduct(WorldPoint - Sd.Start, Sd.Dir);
	};

	// --- emit -------------------------------------------------------------------------------------
	FRammsPointWriter Primitives, Props, Pieces, BlocksOut, SidewalksOut, Features;
	const FString MatRoad = RammsPCG::Mat(Palette, RammsPaletteSlots::MatAsphalt);
	const FString MatWalk = RammsPCG::Mat(Palette, RammsPaletteSlots::MatSidewalk);
	const FString MatPaint = RammsPCG::Mat(Palette, RammsPaletteSlots::MatPaint);
	const FString MatMarking = RammsPCG::Mat(Palette, RammsPaletteSlots::MatMarking);
	const FString MatMarkingYellow = RammsPCG::Mat(Palette, RammsPaletteSlots::MatMarkingYellow);
	const FString MatWarn = RammsPCG::Mat(Palette, RammsPaletteSlots::MatWarning);
	const FString MatLawn = RammsPCG::Mat(Palette, RammsPaletteSlots::MatLawn);
	const FString MatConcrete = RammsPCG::Mat(Palette, RammsPaletteSlots::MatConcrete);
	const FString MatCurb = Palette && Palette->HasSlot(RammsPaletteSlots::MatCurb) ? RammsPCG::Mat(Palette, RammsPaletteSlots::MatCurb) : MatConcrete;
	int32 FeatureSeed = 0;
	auto NextSeed = [&]() { return RammsPCG::SubSeed(Seed, ++FeatureSeed); };

	const double RampWidth = FMath::Clamp(CW * 0.45, 100.0, 150.0);
	const double RampRunMax = CH / FMath::Max(S->RampMaxSlope, 0.02);            // a compliant ramp's run
	const double FlareRunMax = CH / FMath::Max(S->CurbRampFlareSlope, 0.02);    // and its flares' width
	// Crossings near an intersection are set back from the corner far enough that the ramp and its flares
	// clear the perpendicular sidewalk's ramp (which cuts RampRunMax into the corner from the other curb).
	const double CrossOff = FMath::Max(CW * 0.5 + 40.0, FMath::Min(RampRunMax, SW) + RampWidth * 0.5 + FlareRunMax + 30.0);
	// Cuts in the sidewalk ring where a ramp and its flares replace the walk (world XY boxes).
	TArray<FBox2D> RampCuts;
	struct FRampEnd { bool bRamp = false; bool bCompliant = false; };

	// Crossings: one near each end of every live segment, plus optional mid-block. A crossing is only built
	// where it joins two sidewalks (the site-edge roads have a block on one side only). bCorner: an intersection
	// crossing (near a segment's end), recorded on its sides for the hazard placer's corner signals.
	const double Landing = 90.0; // level space kept at the top of a ramp (ADA 406.4: 36 in)
	auto EmitCrossing = [&](const FSeg& Sg, double Pos, bool bCorner)
	{
		const bool bRoadAlongX = Sg.bAlongX;
		const double Across = bRoadAlongX ? Y[Sg.J] : X[Sg.I];
		FSide* SideAt[2] = { nullptr, nullptr };
		double AlongAt[2] = { 0.0, 0.0 };
		FVector CurbAt[2];
		for (int32 k = 0; k < 2; ++k)
		{
			FSide* Sd = FindSide(bRoadAlongX, Across, Pos, k == 0);
			if (!Sd) { return; }
			CurbAt[k] = bRoadAlongX ? FVector(Pos, Across + (k == 0 ? -RW * 0.5 : RW * 0.5), Z0) : FVector(Across + (k == 0 ? -RW * 0.5 : RW * 0.5), Pos, Z0);
			AlongAt[k] = AlongOnSide(*Sd, CurbAt[k]);
			if (AlongAt[k] < RampWidth || AlongAt[k] > Sd->Length - RampWidth) { return; }
			SideAt[k] = Sd;
		}
		// Zebra bars: long along the road (CW), stacked across the road from curb to curb.
		const double BarW = 40.0, Gap = 40.0;
		const int32 Bars = FMath::Max(1, FMath::FloorToInt((RW - 2.0 * 30.0) / (BarW + Gap)));
		const double Span = Bars * (BarW + Gap) - Gap;
		for (int32 b = 0; b < Bars; ++b)
		{
			const double Off = -Span * 0.5 + BarW * 0.5 + b * (BarW + Gap);
			const FVector C = bRoadAlongX ? FVector(Pos, Across + Off, Zr + 0.3) : FVector(Across + Off, Pos, Zr + 0.3);
			if (bGround)
			{
				// On the graded, crowned road, lying in its plane.
				RammsGround::WriteCubeOn(Primitives, FVector(C.X, C.Y, Gm.RoadZ(C.X, C.Y) + 0.3), bRoadAlongX ? FVector(CW, BarW, 0.6) : FVector(BarW, CW, 0.6), 0.0, Gm.Normal(C.X, C.Y), MatPaint, NextSeed());
			}
			else
			{
				RammsPCG::WriteCube(Primitives, C, bRoadAlongX ? FVector(CW, BarW, 0.6) : FVector(BarW, CW, 0.6), 0.0, MatPaint, NextSeed());
			}
		}
		// Curb ramps at both ends.
		FRampEnd Ends[2];
		for (int32 k = 0; k < 2; ++k)
		{
			FSide* Sd = SideAt[k];
			const FVector CurbPoint = CurbAt[k];
			const double Along = AlongAt[k];
			// Whatever is built at this end, nothing stands in front of the crossing: no props, no parked car.
			Sd->Clear.Add(FVector2D(Along - CW * 0.5 - 40.0, Along + CW * 0.5 + 40.0));
			if (bCorner) { Sd->CornerCrossings.Add(Sd->Clear.Last()); }
			if (CH < 1.0)
			{
				// No curb to speak of: a flush crossing.
				Ends[k].bRamp = true; Ends[k].bCompliant = true;
				continue;
			}

			// The curb height here: on graded ground, this block's (it varies along the curb).
			const int32 Bk = Sd->BlockId;
			const double Hc = bGround ? Gm.Reveal(Bk, CurbPoint.X, CurbPoint.Y) : CH;
			const FVector CurbTop = bGround ? FVector(CurbPoint.X, CurbPoint.Y, Gm.BlockZ(Bk, CurbPoint.X, CurbPoint.Y)) : CurbPoint;
			const double Roll = Rng.FRand();
			const bool bBreak = Roll < S->NonCompliance;
			const double Sub = Rng.FRand();
			if (bBreak && Sub < 0.5)
			{
				// Missing curb ramp: the curb runs through. Record it; nothing to build.
				const int32 f = Features.AddBox(CurbTop, Sd->Yaw, FVector(RampWidth, 20.0, Hc), NextSeed());
				RammsPCG::WriteManifest(Features, f, FName(TEXT("Crossing")), FName(TEXT("MissingCurbRamp")), false, ERammsComplianceIssue::MissingCurbRamp, Hc, S->VerticalChangeMax);
				continue;
			}
			FRammsPieceDesc P;
			P.Kind = ERammsProcPieceKind::CurbRamp;
			P.Rise = Hc;
			P.SizeY = RampWidth;
			P.FlareSlope = S->CurbRampFlareSlope;
			P.MaterialPath = MatConcrete;
			P.MaterialPath2 = MatWarn;
			P.ManifestKind = FName(TEXT("CurbRamp"));
			P.Threshold = S->RampMaxSlope;
			P.Seed = NextSeed();
			if (bBreak && Sub < 0.85)
			{
				P.Slope = S->RampMaxSlope * RammsPCG::RandRange(Rng, 1.4, 2.4);
				P.WarningDepth = S->DetectableWarningDepth;
				P.bCompliant = false; P.Issue = ERammsComplianceIssue::SlopeTooSteep; P.ManifestSubtype = FName(TEXT("TooSteep"));
			}
			else if (bBreak)
			{
				P.Slope = S->RampMaxSlope;
				P.WarningDepth = 0.0; // compliant for a wheelchair, flagged for the record
				P.bCompliant = true; P.ManifestSubtype = FName(TEXT("NoDetectableWarning"));
			}
			else
			{
				P.Slope = S->RampMaxSlope;
				P.WarningDepth = S->DetectableWarningDepth;
				P.ManifestSubtype = FName(TEXT("Standard"));
			}
			// On a narrow sidewalk the ramp is shortened to keep a landing at its top, which steepens it: then it
			// is recorded as too steep, whatever it was meant to be.
			double Run = 0.0;
			double SlopeIn = 0.0, SlopeAlong = 0.0;
			if (bGround)
			{
				// The walk's own slopes at the ramp (the hill, the curb's change along the block, the cross slope): it climbs
				// SlopeIn per cm inward from the curb, so a ramp of horizontal run Run climbs Hc + SlopeIn * Run to meet it.
				const FVector Mid = CurbPoint + Sd->Inward * FMath::Min(SW * 0.5, 90.0);
				auto Wz = [&](const FVector& Q) { return Gm.BlockZ(Bk, Q.X, Q.Y); };
				SlopeIn = (Wz(Mid + Sd->Inward * 10.0) - Wz(Mid - Sd->Inward * 10.0)) / 20.0;
				SlopeAlong = (Wz(Mid + Sd->Dir * 10.0) - Wz(Mid - Sd->Dir * 10.0)) / 20.0;
				const double Want = Hc / FMath::Max(P.Slope - SlopeIn, 0.01);
				Run = FMath::Max(FMath::Min(Want, SW - Landing), FMath::Min(SW * 0.5, Want));
				const double Actual = Hc / Run + SlopeIn;
				// The piece's own slope: it is tilted onto the walk's plane on top of it.
				P.Slope = Hc / FMath::Max(Run, 1.0);
				if (P.bCompliant && Actual > S->RampMaxSlope * 1.01)
				{
					P.bCompliant = false; P.Issue = ERammsComplianceIssue::SlopeTooSteep; P.ManifestSubtype = FName(TEXT("TooSteep"));
				}
				P.Measure = Actual;
				// On a grade the ramp's cross slope, and its top landing's, is the street's: above MaxCrossSlope it is recorded.
				if (P.bCompliant && FMath::Abs(SlopeAlong) > S->MaxCrossSlope + 1e-3)
				{
					P.bCompliant = false; P.Issue = ERammsComplianceIssue::CrossSlopeTooSteep; P.ManifestSubtype = FName(TEXT("CrossSlope"));
					P.Measure = FMath::Abs(SlopeAlong); P.Threshold = S->MaxCrossSlope;
				}
			}
			else
			{
				Run = FMath::Max(FMath::Min(CH / P.Slope, SW - Landing), FMath::Min(SW * 0.5, CH / P.Slope));
				P.Slope = CH / Run;
				if (P.bCompliant && P.Slope > S->RampMaxSlope * 1.01)
				{
					P.bCompliant = false; P.Issue = ERammsComplianceIssue::SlopeTooSteep; P.ManifestSubtype = FName(TEXT("TooSteep"));
				}
				P.Measure = P.Slope;
			}
			// Flares: as wide as the slope allows, but never past the end of the side (the corner).
			const double Flare = FMath::Clamp(FMath::Min(Along, Sd->Length - Along) - RampWidth * 0.5, 0.0, FlareRunMax);
			P.FlareSlope = Flare > 5.0 ? Hc / Flare : 100.0;
			// Ramp frame: +X points outward (descends to the road); origin at the top, Run inward from the curb.
			const FVector Origin = CurbPoint + Sd->Inward * Run;
			if (bGround)
			{
				// Tilted onto the walk's plane: +X outward falling with it, +Y along the curb climbing with the street. The
				// ramp then drops Hc more to meet the gutter, and its flares meet the walk.
				const FVector XDir = -Sd->Inward + FVector(0.0, 0.0, -SlopeIn);
				const FVector YDir = Sd->Dir + FVector(0.0, 0.0, SlopeAlong);
				const FVector Nrm = FVector::CrossProduct(XDir, YDir).GetSafeNormal();
				P.Transform = FTransform(RammsGround::FromAxes(XDir, Nrm), FVector(Origin.X, Origin.Y, Gm.BlockZ(Bk, Origin.X, Origin.Y)));
			}
			else
			{
				P.Transform = FTransform(FRotator(0.0, Sd->Yaw - 90.0, 0.0), Origin);
			}
			RammsPCG::WritePiece(Pieces, P);
			Ends[k].bRamp = true;
			Ends[k].bCompliant = P.bCompliant;
			const double Half = RampWidth * 0.5 + (Flare > 5.0 ? Flare : 0.0);
			// The ramp and flares replace the walk and any planting strip from the curb to the top of the ramp; a
			// strip wider than the ramp's run is paved over the rest, so the ramp top meets the walk directly.
			const double CutDepth = FMath::Max(Run, Sd->StripDepth);
			{
				const FVector C0 = CurbPoint - Sd->Dir * Half, C1 = CurbPoint + Sd->Dir * Half + Sd->Inward * CutDepth;
				RampCuts.Add(FBox2D(FVector2D(FMath::Min(C0.X, C1.X), FMath::Min(C0.Y, C1.Y)), FVector2D(FMath::Max(C0.X, C1.X), FMath::Max(C0.Y, C1.Y))));
			}
			if (CutDepth > Run + 1.0)
			{
				if (bGround)
				{
					// The level walk between the ramp's top and the back of the cut, on the walk's plane.
					const FVector C3 = CurbPoint + Sd->Inward * ((Run + CutDepth) * 0.5);
					RammsGround::FPrisms Fill(FVector(C3.X, C3.Y, Gm.BlockZ(Bk, C3.X, C3.Y)));
					const FBox2D Local(FVector2D(Along - Half, Run), FVector2D(Along + Half, CutDepth));
					for (const TArray<FVector2D>& Pg : SidePolys(RefOf(*Sd), Local))
					{
						Fill.AddPoly(Pg, [&](const FVector2D& Q) { return Gm.BlockZ(Bk, Q.X, Q.Y); }, 0, SurfConcrete);
					}
					FRammsPieceDesc D = Fill.Desc(20.0);
					D.MaterialPath = MatWalk;
					D.ManifestSubtype = RammsGround::GroundSubtype;
					D.Seed = NextSeed();
					RammsPCG::WritePiece(Pieces, D);
				}
				else
				{
					RammsPCG::WriteCube(Primitives, CurbPoint + Sd->Inward * ((Run + CutDepth) * 0.5) + FVector(0, 0, -10.0), FVector(2.0 * Half, CutDepth - Run, 20.0), Sd->Yaw, MatWalk, NextSeed());
				}
			}
			// Nothing else on the ramp, its flares or the landing at the top: no curb, props or parked cars.
			Sd->Keepouts.Add(FVector2D(FMath::Max(Along - Half - 10.0, 0.0), FMath::Min(Along + Half + 10.0, Sd->Length)));
			Sd->Clear.Add(Sd->Keepouts.Last());
			if (bCorner)
			{
				FVector2D& Cc = Sd->CornerCrossings.Last();
				Cc = FVector2D(FMath::Min(Cc.X, Sd->Keepouts.Last().X), FMath::Max(Cc.Y, Sd->Keepouts.Last().Y));
			}
		}
		// The crossing itself, curb to curb: crowd lanes and route planners use it. The subtype says whether a
		// wheelchair can get on and off it without climbing a curb.
		{
			FVector C = bRoadAlongX ? FVector(Pos, Across, Zr) : FVector(Across, Pos, Zr);
			if (bGround) { C.Z = Gm.RoadZ(C.X, C.Y); }
			const int32 f = Features.AddBox(C, bRoadAlongX ? 90.0 : 0.0, FVector(RW, CW, 1.0), NextSeed());
			const int32 Ramps = (Ends[0].bRamp ? 1 : 0) + (Ends[1].bRamp ? 1 : 0);
			const bool bAllCompliant = Ends[0].bCompliant && Ends[1].bCompliant;
			RammsPCG::WriteManifest(Features, f, FName(TEXT("Crosswalk")),
				CH < 1.0 ? FName(TEXT("Flush")) : Ramps == 2 ? (bAllCompliant ? FName(TEXT("Ramped")) : FName(TEXT("SteepRamp"))) : Ramps == 1 ? FName(TEXT("OneRamp")) : FName(TEXT("NoRamps")),
				true, ERammsComplianceIssue::None, RW, 0.0);
		}
	};

	// Speed humps and tables (world plan boxes, graded ground): parked cars keep off them.
	TArray<FBox2D> HumpBoxes;
	for (const FSeg& Sg : Segs)
	{
		if (!Sg.bLive) { continue; }
		const double A = Sg.bAlongX ? X[Sg.I] : Y[Sg.J];
		const double B = Sg.bAlongX ? X[Sg.I + 1] : Y[Sg.J + 1];
		const double Across = Sg.bAlongX ? Y[Sg.J] : X[Sg.I];
		const double Len = B - A - RW; // between intersection squares
		// Road slab between the intersection squares (graded: built below, once the crossings are known).
		const FVector C = Sg.bAlongX ? FVector((A + B) * 0.5, Across, Zr - 10.0) : FVector(Across, (A + B) * 0.5, Zr - 10.0);
		int32 RoadSeed = 0;
		if (bGround)
		{
			RoadSeed = NextSeed();
		}
		else
		{
			RammsPCG::WriteCube(Primitives, C, Sg.bAlongX ? FVector(Len, RW, 20.0) : FVector(RW, Len, 20.0), 0.0, MatRoad, NextSeed());
		}
		// Road wear: cracks, patches, manholes and the odd pothole, projected onto the asphalt.
		if (S->DecalDensity > 0.0 && Palette && Len > 300.0)
		{
			const int32 N = FMath::RoundToInt(S->DecalDensity * Len / 10000.0 + RammsPCG::RandRange(Rng, -0.4, 0.4));
			for (int32 n = 0; n < N; ++n)
			{
				const double Roll = Rng.FRand();
				const FName Slot = Roll < 0.45 ? RammsPaletteSlots::DecalRoadCrack : Roll < 0.7 ? RammsPaletteSlots::DecalPatch : Roll < 0.85 ? RammsPaletteSlots::DecalPothole : RammsPaletteSlots::DecalPuddle;
				const FString M = Palette->PickMaterialPath(Slot, Rng);
				if (M.IsEmpty()) { continue; }
				const double Sz = Slot == RammsPaletteSlots::DecalPothole ? RammsPCG::RandRange(Rng, 70.0, 140.0)
					: Slot == RammsPaletteSlots::DecalPatch ? RammsPCG::RandRange(Rng, 80.0, 170.0) : RammsPCG::RandRange(Rng, 150.0, 400.0);
				const double Along = RammsPCG::RandRange(Rng, A + RW * 0.5 + 100.0, B - RW * 0.5 - 100.0);
				const double AcrossD = RammsPCG::RandRange(Rng, -RW * 0.5 + Sz * 0.4, RW * 0.5 - Sz * 0.4);
				FRammsPieceDesc Dc;
				Dc.Kind = ERammsProcPieceKind::Decal;
				Dc.SizeX = Sz; Dc.SizeY = Sz * RammsPCG::RandRange(Rng, 0.5, 1.0); Dc.SizeZ = 40.0;
				Dc.MaterialPath = M;
				FVector P = Sg.bAlongX ? FVector(Along, Y[Sg.J] + AcrossD, Zr) : FVector(X[Sg.I] + AcrossD, Along, Zr);
				if (bGround) { P.Z = Gm.RoadZ(P.X, P.Y); }
				Dc.Transform = FTransform(FRotator(0.0, RammsPCG::RandRange(Rng, 0.0, 360.0), 0.0), P);
				Dc.Seed = NextSeed();
				RammsPCG::WritePiece(Pieces, Dc);
			}
		}
		// Which crossings this segment gets (decided before the markings, which leave them clear).
		const bool bEnds = Len > 2.0 * (CrossOff + CW * 0.5) + 200.0;
		const bool bMid = bEnds ? (Len > 3000.0 && Rng.FRand() < S->MidblockCrossingChance) : Len > CW;
		const double MidPos = (A + B) * 0.5;
		const double MidHalf = CW * 0.5 + 80.0; // the mid-block crossing and its stop lines, kept free of lane markings
		// Markings: a dashed yellow centre line between the crossings, white edge lines along the
		// gutters, and a stop line before each crossing.
		{
			const double a0 = A + RW * 0.5, b0 = B - RW * 0.5;
			const double Keep = CrossOff + CW * 0.5 + 80.0; // leave the crossings and stop lines clear
			auto Along = [&](double Pos, double Off, double LenAlong, double Wide, const FString& Mat)
			{
				const FVector P = Sg.bAlongX ? FVector(Pos, Across + Off, Zr + 0.3) : FVector(Across + Off, Pos, Zr + 0.3);
				if (bGround)
				{
					RammsGround::WriteCubeOn(Primitives, FVector(P.X, P.Y, Gm.RoadZ(P.X, P.Y) + 0.3), Sg.bAlongX ? FVector(LenAlong, Wide, 0.6) : FVector(Wide, LenAlong, 0.6), 0.0, Gm.Normal(P.X, P.Y), Mat, NextSeed());
					return;
				}
				RammsPCG::WriteCube(Primitives, P, Sg.bAlongX ? FVector(LenAlong, Wide, 0.6) : FVector(Wide, LenAlong, 0.6), 0.0, Mat, NextSeed());
			};
			const bool bMidGap = bMid && bEnds;
			const double StopHand = Sg.bAlongX ? 1.0 : -1.0; // the side of the road a car heading +along keeps to
			if (RW >= 500.0)
			{
				for (double p = a0 + Keep; p + 300.0 <= b0 - Keep; p += 600.0)
				{
					if (bMidGap && p + 300.0 > MidPos - MidHalf && p < MidPos + MidHalf) { continue; }
					Along(p + 150.0, 0.0, 300.0, 12.0, MatMarkingYellow);
				}
			}
			if (b0 - a0 > 2.0 * Keep + 200.0)
			{
				// Edge lines, in two pieces around a mid-block crossing.
				TArray<FVector2D> Runs;
				if (bMidGap) { Runs = { FVector2D(a0 + Keep, MidPos - MidHalf), FVector2D(MidPos + MidHalf, b0 - Keep) }; }
				else { Runs = { FVector2D(a0 + Keep, b0 - Keep) }; }
				for (const FVector2D& Rn : Runs)
				{
					if (Rn.Y - Rn.X < 100.0) { continue; }
					Along((Rn.X + Rn.Y) * 0.5, RW * 0.5 - 30.0, Rn.Y - Rn.X, 10.0, MatMarking);
					Along((Rn.X + Rn.Y) * 0.5, -RW * 0.5 + 30.0, Rn.Y - Rn.X, 10.0, MatMarking);
				}
			}
			if (Len > 2.0 * (CrossOff + CW * 0.5) + 400.0)
			{
				// Stop lines sit before each crossing (away from the intersection), on the approach half of the road:
				// right-hand traffic, so the half on the right of a car heading for the crossing. Unreal is left-handed:
				// heading -X the right is -Y, but heading -Y it is +X, so the half's sign flips with the road's axis.
				Along(a0 + CrossOff + CW * 0.5 + 55.0, -StopHand * RW * 0.25, 30.0, RW * 0.5 - 40.0, MatMarking);
				Along(b0 - CrossOff - CW * 0.5 - 55.0, StopHand * RW * 0.25, 30.0, RW * 0.5 - 40.0, MatMarking);
			}
			if (bMidGap)
			{
				// Mid-block: before the crossing for each direction (the A side is approached heading toward B).
				Along(MidPos - CW * 0.5 - 55.0, StopHand * RW * 0.25, 30.0, RW * 0.5 - 40.0, MatMarking);
				Along(MidPos + CW * 0.5 + 55.0, -StopHand * RW * 0.25, 30.0, RW * 0.5 - 40.0, MatMarking);
			}
		}
		// Crossings near both ends, and mid-block.
		if (bEnds)
		{
			EmitCrossing(Sg, A + RW * 0.5 + CrossOff, true);
			EmitCrossing(Sg, B - RW * 0.5 - CrossOff, true);
		}
		if (bMid)
		{
			EmitCrossing(Sg, MidPos, false);
		}
		if (bGround)
		{
			// The road between the intersection squares: its two crowned halves (each one plane), less the potholes cut
			// into them, and now and then a speed hump or table across it. Road frame: U along the road, V across it.
			const double a0 = A + RW * 0.5, b0 = B - RW * 0.5;
			const double Hh = RW * 0.5;
			auto W2 = [&](double U, double V) { return Sg.bAlongX ? FVector2D(U, Across + V) : FVector2D(Across + V, U); };
			auto RoadPoly = [&](const FBox2D& R) { return TArray<FVector2D>{ W2(R.Min.X, R.Min.Y), W2(R.Max.X, R.Min.Y), W2(R.Max.X, R.Max.Y), W2(R.Min.X, R.Max.Y) }; };
			auto RoadH = [&](const FVector2D& P) { return Gm.RoadZ(P.X, P.Y); };
			TArray<double> Xings;
			if (bEnds) { Xings.Add(a0 + CrossOff); Xings.Add(b0 - CrossOff); }
			if (bMid) { Xings.Add(MidPos); }
			// A stretch of road clear of the crossings (and their stop lines) and of the intersections.
			auto ClearOf = [&](double P0, double P1)
			{
				if (P0 < a0 + 80.0 || P1 > b0 - 80.0) { return false; }
				for (const double Xp : Xings) { if (P1 > Xp - CW * 0.5 - 120.0 && P0 < Xp + CW * 0.5 + 120.0) { return false; } }
				return true;
			};
			double Hump0 = 0.0, Hump1 = -1.0;
			if (S->SpeedHumpChance > 0.0 && GRng.FRand() < S->SpeedHumpChance)
			{
				const bool bTable = GRng.FRand() < 0.4;
				const double HL = bTable ? RammsPCG::RandRange(GRng, 600.0, 700.0) : RammsPCG::RandRange(GRng, 360.0, 420.0);
				const double HH = bTable ? RammsPCG::RandRange(GRng, 7.5, 9.0) : RammsPCG::RandRange(GRng, 7.0, 10.0);
				const double Plateau = bTable ? HL - 2.0 * RammsPCG::RandRange(GRng, 150.0, 190.0) : 0.0;
				for (int32 Try = 0; Try < 8 && Hump1 < Hump0; ++Try)
				{
					const double U0 = RammsPCG::RandRange(GRng, a0 + 200.0, FMath::Max(a0 + 200.0, b0 - 200.0 - HL));
					if (ClearOf(U0, U0 + HL)) { Hump0 = U0; Hump1 = U0 + HL; }
				}
				if (Hump1 > Hump0)
				{
					const FVector Dir3 = Sg.bAlongX ? FVector(1.0, 0.0, 0.0) : FVector(0.0, 1.0, 0.0);
					for (int32 Side2 = 0; Side2 < 2; ++Side2)
					{
						// One piece per half, lying on its plane; the profile runs along the road.
						const double Vc = Side2 == 0 ? -Hh * 0.5 : Hh * 0.5;
						const FVector2D O2 = W2(Hump0, Vc), O3 = W2(Hump0 + 10.0, Vc);
						const double Zs = Gm.RoadZ(O2.X, O2.Y);
						const double Slope = (Gm.RoadZ(O3.X, O3.Y) - Zs) / 10.0;
						const FVector2D Cm = W2((Hump0 + Hump1) * 0.5, Vc);
						FRammsPieceDesc Hp;
						Hp.Kind = ERammsProcPieceKind::Hump;
						Hp.SizeX = HL; Hp.SizeY = Hh; Hp.SizeZ = 10.0; Hp.Rise = HH; Hp.LandingSize = Plateau;
						Hp.MaterialPath = MatRoad;
						Hp.Transform = FTransform(RammsGround::FromAxes(Dir3 + FVector(0.0, 0.0, Slope), Gm.Normal(Cm.X, Cm.Y)), FVector(O2.X, O2.Y, Zs));
						if (Side2 == 0)
						{
							Hp.ManifestKind = FName(TEXT("SpeedHump"));
							Hp.ManifestSubtype = bTable ? FName(TEXT("Table")) : FName(TEXT("Hump"));
							Hp.Measure = HH;
						}
						else
						{
							Hp.ManifestSubtype = RammsGround::GroundSubtype;
						}
						Hp.Seed = NextGSeed();
						RammsPCG::WritePiece(Pieces, Hp);
					}
					FBox2D Hb(ForceInit);
					Hb += W2(Hump0, -Hh);
					Hb += W2(Hump1, Hh);
					HumpBoxes.Add(Hb);
				}
			}
			// Potholes: real holes, the road cut open around each one and its floor 2.5-9 cm down.
			TArray<FBox2D> Holes[2];
			TArray<double> HoleDepth[2];
			if (S->PotholeDensity > 0.0 && b0 - a0 > 400.0)
			{
				const int32 NH = FMath::RoundToInt(S->PotholeDensity * (b0 - a0) / 10000.0 + RammsPCG::RandRange(GRng, -0.4, 0.4));
				for (int32 n = 0; n < NH; ++n)
				{
					const int32 Hf = GRng.FRand() < 0.5 ? 0 : 1;
					const double Wu = RammsPCG::RandRange(GRng, 40.0, 120.0), Wv = RammsPCG::RandRange(GRng, 30.0, 90.0);
					const double U0 = RammsPCG::RandRange(GRng, a0 + 80.0, FMath::Max(a0 + 80.0, b0 - 80.0 - Wu));
					const double VLo = Hf == 0 ? -Hh + 25.0 : 25.0, VHi = Hf == 0 ? -25.0 - Wv : Hh - 25.0 - Wv;
					const double V0 = RammsPCG::RandRange(GRng, VLo, FMath::Max(VLo, VHi));
					const double Depth = RammsPCG::RandRange(GRng, 2.5, 9.0);
					if (VHi < VLo || !ClearOf(U0, U0 + Wu)) { continue; }
					if (Hump1 > Hump0 && U0 + Wu > Hump0 - 60.0 && U0 < Hump1 + 60.0) { continue; }
					const FBox2D Hb(FVector2D(U0, V0), FVector2D(U0 + Wu, V0 + Wv));
					bool bFree = true;
					for (const FBox2D& O : Holes[Hf]) { if (RammsPCG::Overlaps(O.ExpandBy(30.0), Hb)) { bFree = false; break; } }
					if (!bFree) { continue; }
					Holes[Hf].Add(Hb);
					HoleDepth[Hf].Add(Depth);
				}
			}
			const FVector2D Mc = W2((a0 + b0) * 0.5, 0.0);
			RammsGround::FPrisms Road(FVector(Mc.X, Mc.Y, Gm.RoadZ(Mc.X, Mc.Y)));
			for (int32 Hf = 0; Hf < 2; ++Hf)
			{
				TArray<FBox2D> Rects = { FBox2D(FVector2D(a0, Hf == 0 ? -Hh : 0.0), FVector2D(b0, Hf == 0 ? 0.0 : Hh)) };
				for (const FBox2D& Hb : Holes[Hf])
				{
					TArray<FBox2D> Next;
					for (const FBox2D& R : Rects) { RammsGround::SubtractBox(R, Hb, Next); }
					Rects = MoveTemp(Next);
				}
				for (const FBox2D& R : Rects) { Road.AddPoly(RoadPoly(R), RoadH, 0, SurfAsphalt); }
				for (int32 h = 0; h < Holes[Hf].Num(); ++h)
				{
					const FBox2D& Hb = Holes[Hf][h];
					const double Depth = HoleDepth[Hf][h];
					const FVector2D Hc2 = W2(Hb.GetCenter().X, Hb.GetCenter().Y);
					RammsGround::FPrisms Floor(FVector(Hc2.X, Hc2.Y, Gm.RoadZ(Hc2.X, Hc2.Y) - Depth));
					Floor.AddPoly(RoadPoly(Hb), [&](const FVector2D& P) { return Gm.RoadZ(P.X, P.Y) - Depth; }, 0, SurfAsphalt);
					FRammsPieceDesc D = Floor.Desc(FMath::Max(20.0 - Depth, 4.0));
					D.MaterialPath = MatRoad;
					D.ManifestKind = FName(TEXT("Surface"));
					D.ManifestSubtype = FName(TEXT("Pothole"));
					D.Measure = Depth;
					D.Threshold = S->VerticalChangeMax;
					D.bCompliant = false;
					D.Issue = ERammsComplianceIssue::VerticalChangeTooHigh;
					D.Seed = NextGSeed();
					RammsPCG::WritePiece(Pieces, D);
				}
			}
			FRammsPieceDesc D = Road.Desc(20.0);
			D.MaterialPath = MatRoad;
			D.ManifestSubtype = RammsGround::GroundSubtype;
			D.Seed = RoadSeed;
			RammsPCG::WritePiece(Pieces, D);
		}
	}
	// Intersection squares at nodes touched by a live segment.
	for (int32 j = 0; j < NY; ++j)
	{
		for (int32 i = 0; i < NX; ++i)
		{
			bool bAny = false;
			if (i > 0) { bAny |= Segs[SegIndexX(i - 1, j)].bLive; }
			if (i + 1 < NX) { bAny |= Segs[SegIndexX(i, j)].bLive; }
			if (j > 0) { bAny |= Segs[SegIndexY(i, j - 1)].bLive; }
			if (j + 1 < NY) { bAny |= Segs[SegIndexY(i, j)].bLive; }
			if (bAny && bGround)
			{
				// Graded and crowned: eight planar triangles, each half of the triangle toward an edge carrying the crown
				// of the road through that edge (or of the other road where the edge is a curb).
				const double Hh = RW * 0.5;
				const FVector2D Cn(X[i], Y[j]);
				const FVector2D Corner[4] = { Cn + FVector2D(Hh, -Hh), Cn + FVector2D(Hh, Hh), Cn + FVector2D(-Hh, Hh), Cn + FVector2D(-Hh, -Hh) };
				RammsGround::FPrisms Sq(FVector(Cn.X, Cn.Y, Gm.RoadZ(Cn.X, Cn.Y)));
				auto RoadH = [&](const FVector2D& P) { return Gm.RoadZ(P.X, P.Y); };
				for (int32 e = 0; e < 4; ++e)
				{
					const FVector2D& Pa = Corner[e];
					const FVector2D& Pb = Corner[(e + 1) % 4];
					const FVector2D Edge = (Pa + Pb) * 0.5;
					Sq.AddPoly({ Cn, Pa, Edge }, RoadH, 0, SurfAsphalt);
					Sq.AddPoly({ Cn, Edge, Pb }, RoadH, 0, SurfAsphalt);
				}
				FRammsPieceDesc D = Sq.Desc(20.0);
				D.MaterialPath = MatRoad;
				D.ManifestSubtype = RammsGround::GroundSubtype;
				D.Seed = NextSeed();
				RammsPCG::WritePiece(Pieces, D);
			}
			else if (bAny)
			{
				RammsPCG::WriteCube(Primitives, FVector(X[i], Y[j], Zr - 10.0), FVector(RW, RW, 20.0), 0.0, MatRoad, NextSeed());
			}
		}
	}
	// Road centrelines.
	if (bGround)
	{
		// On the crown, through every intersection.
		for (int32 j = 0; j < NY; ++j)
		{
			TArray<FVector> Line;
			for (int32 i = 0; i < NX; ++i) { Line.Add(FVector(X[i], Y[j], Gm.RoadZ(X[i], Y[j]))); }
			RammsPCG::EmitSpline(Context, RammsPCG::PinRoads, Line, false, { TEXT("RoadX") });
		}
		for (int32 i = 0; i < NX; ++i)
		{
			TArray<FVector> Line;
			for (int32 j = 0; j < NY; ++j) { Line.Add(FVector(X[i], Y[j], Gm.RoadZ(X[i], Y[j]))); }
			RammsPCG::EmitSpline(Context, RammsPCG::PinRoads, Line, false, { TEXT("RoadY") });
		}
	}
	else
	{
		for (int32 j = 0; j < NY; ++j) { RammsPCG::EmitSpline(Context, RammsPCG::PinRoads, { FVector(X[0], Y[j], Zr), FVector(X.Last(), Y[j], Zr) }, false, { TEXT("RoadX") }); }
		for (int32 i = 0; i < NX; ++i) { RammsPCG::EmitSpline(Context, RammsPCG::PinRoads, { FVector(X[i], Y[0], Zr), FVector(X[i], Y.Last(), Zr) }, false, { TEXT("RoadY") }); }
	}

	// Sidewalk rings, curbs, blocks, sidewalk-side points.
	FRammsPaletteMesh CurbMesh;
	const bool bCurbMesh = Palette && Palette->PickMesh(RammsPaletteSlots::Curb, Rng, CurbMesh);
	const double CurbStep = bCurbMesh ? FMath::Max(CurbMesh.Footprint.X, 50.0) : 200.0;
	// A side-local rectangle (X along the side, Y inward from the curb) minus the ramp cuts that touch it.
	auto MinusRampCuts = [&](const FSide& Sd, const FBox2D& Rect) -> TArray<FBox2D>
	{
		TArray<FBox2D> Out = { Rect };
		for (const FBox2D& Cw : RampCuts)
		{
			FBox2D Cut(ForceInit);
			for (int32 c = 0; c < 4; ++c)
			{
				const FVector P((c & 1) ? Cw.Max.X : Cw.Min.X, (c & 2) ? Cw.Max.Y : Cw.Min.Y, Sd.Start.Z);
				Cut += FVector2D(FVector::DotProduct(P - Sd.Start, Sd.Dir), FVector::DotProduct(P - Sd.Start, Sd.Inward));
			}
			TArray<FBox2D> Next;
			for (const FBox2D& R : Out)
			{
				if (!RammsPCG::Overlaps(R, Cut)) { Next.Add(R); continue; }
				// Up to four pieces around the cut: before and after it along the side, then the strips
				// inward and outward of it over the cut's along span.
				const double CA0 = FMath::Max(Cut.Min.X, R.Min.X), CA1 = FMath::Min(Cut.Max.X, R.Max.X);
				if (CA0 - R.Min.X > 1.0) { Next.Add(FBox2D(R.Min, FVector2D(CA0, R.Max.Y))); }
				if (R.Max.X - CA1 > 1.0) { Next.Add(FBox2D(FVector2D(CA1, R.Min.Y), R.Max)); }
				if (Cut.Min.Y - R.Min.Y > 1.0) { Next.Add(FBox2D(FVector2D(CA0, R.Min.Y), FVector2D(CA1, Cut.Min.Y))); }
				if (R.Max.Y - Cut.Max.Y > 1.0) { Next.Add(FBox2D(FVector2D(CA0, Cut.Max.Y), FVector2D(CA1, R.Max.Y))); }
			}
			Out = MoveTemp(Next);
		}
		return Out;
	};
	const TArray<FBox2D> KeepBoxes = RammsPCG::ParseBoxes(S->KeepClear);
	auto InKeep = [&KeepBoxes](const FVector& P, double Margin)
	{
		for (const FBox2D& K : KeepBoxes) { if (K.ExpandBy(Margin).IsInside(FVector2D(P.X, P.Y))) { return true; } }
		return false;
	};

	// --- graded / rough sidewalks (bGround) -------------------------------------------------------------------
	// One side's walk (cast in panels when rough) and planting strip as GroundPrisms pieces on the ground model; each
	// roughness feature a piece of its own carrying its manifest record; and the side's grades and curb heights.
	// A recipe palette need not carry every surface: the first of these slots the palette fills, else the first one's
	// fallback (the palette's default material). With the first slot filled, that is what it always was.
	auto MatFirst = [Palette](const TArray<FName>& Candidates) -> FString
	{
		for (const FName& Cand : Candidates) { if (Palette && Palette->HasSlot(Cand)) { return RammsPCG::Mat(Palette, Cand); } }
		return RammsPCG::Mat(Palette, Candidates.Num() > 0 ? Candidates[0] : NAME_None);
	};
	const FString MatPaver = MatFirst({ RammsGround::MatBrick, RammsPaletteSlots::MatFacade, RammsPaletteSlots::MatConcrete, RammsPaletteSlots::MatSidewalk });
	const FString MatSett = MatFirst({ RammsPaletteSlots::MatCobble, RammsPaletteSlots::MatConcrete, RammsPaletteSlots::MatSidewalk });
	const FString MatCover = MatFirst({ RammsPaletteSlots::MatMetal, RammsPaletteSlots::MatAsphalt, RammsPaletteSlots::MatConcrete });
	auto EmitGroundSide = [&](const FSide& Sd, double SL0, double SL, double Pave0, bool bStripSide, FName Route)
	{
		const int32 Bk = Sd.BlockId;
		const FSideRef Ref = RefOf(Sd);
		const double Rough = FMath::Clamp(S->SurfaceRoughness, 0.0, 1.0);
		const double PL = FMath::Max(S->PanelLength, 60.0);
		const double VMax = S->VerticalChangeMax;
		auto WalkZ = [&](const FVector2D& Q) { return Gm.BlockZ(Bk, Q.X, Q.Y); };
		// A side-local point (along, inward) on the walk; a world point to side-local; side-local polygons to world.
		auto At = [&](double Al, double In) { const FVector Q = Sd.Start + Sd.Dir * Al + Sd.Inward * In; return FVector(Q.X, Q.Y, Gm.BlockZ(Bk, Q.X, Q.Y)); };
		auto ToLocal = [&](const FVector2D& Q) { const FVector D3 = FVector(Q.X, Q.Y, Sd.Start.Z) - Sd.Start; return FVector2D(FVector::DotProduct(D3, Sd.Dir), FVector::DotProduct(D3, Sd.Inward)); };
		auto LocalPoly = [&](const TArray<FVector2D>& Pts)
		{
			TArray<FVector2D> Wps;
			for (const FVector2D& Lp : Pts) { const FVector Q = Sd.Start + Sd.Dir * Lp.X + Sd.Inward * Lp.Y; Wps.Add(FVector2D(Q.X, Q.Y)); }
			return Wps;
		};
		auto WriteFeature = [&](const RammsGround::FPrisms& Pr, const FString& InMat, FName InKind, FName InSub, double InMeasure, double InLimit, bool bOk, ERammsComplianceIssue InIssue)
		{
			FRammsPieceDesc D = Pr.Desc(20.0);
			D.MaterialPath = InMat;
			D.ManifestKind = InKind; D.ManifestSubtype = InSub;
			D.Measure = InMeasure; D.Threshold = InLimit;
			D.bCompliant = bOk; D.Issue = bOk ? ERammsComplianceIssue::None : InIssue;
			D.Seed = NextGSeed();
			RammsPCG::WritePiece(Pieces, D);
		};

		// The planting strip: lawn, 5 mm under the walk as on the flat streets.
		if (bStripSide)
		{
			RammsGround::FPrisms Strip(At(SL0 + SL * 0.5, FZ * 0.5));
			for (const FBox2D& Rc : MinusRampCuts(Sd, FBox2D(FVector2D(SL0, 0.0), FVector2D(SL0 + SL, FZ))))
			{
				for (const TArray<FVector2D>& Pg : SidePolys(Ref, Rc)) { Strip.AddPoly(Pg, [&](const FVector2D& Q) { return WalkZ(Q) - 0.5; }, 0, SurfGrass); }
			}
			if (!Strip.IsEmpty())
			{
				FRammsPieceDesc D = Strip.Desc(20.0);
				D.MaterialPath = MatLawn; D.UVScale = 350.0; D.ManifestSubtype = RammsGround::GroundSubtype; D.Seed = NextGSeed();
				RammsPCG::WritePiece(Pieces, D);
			}
		}

		// Panels: rough walks are cast in panels PanelLength long (the furniture zone its own band), each set a little
		// high or low; panels a curb ramp's pour touches are level with it. Smooth walks are one panel per cut piece.
		struct FTile { FBox2D R = FBox2D(ForceInit); double Dz = 0.0; int32 Band = 0; bool bWhole = true; int32 Special = 0; };
		TArray<FTile> Tiles;
		TArray<FVector2D> Bands;
		if (bRough && !bStripSide && FZ > 30.0 && FZ < SW - 30.0) { Bands.Add(FVector2D(Pave0, FZ)); Bands.Add(FVector2D(FZ, SW)); }
		else { Bands.Add(FVector2D(Pave0, SW)); }
		auto IsItself = [](const TArray<FBox2D>& Got, const FBox2D& Want) { return Got.Num() == 1 && Got[0].Min.Equals(Want.Min, 0.01) && Got[0].Max.Equals(Want.Max, 0.01); };
		for (int32 b = 0; b < Bands.Num(); ++b)
		{
			TArray<double> Joints;
			Joints.Add(SL0);
			if (bRough)
			{
				for (double Ja = (FMath::FloorToDouble(SL0 / PL) + 1.0) * PL; Ja < SL0 + SL - 20.0; Ja += PL)
				{
					if (Ja > Joints.Last() + 20.0) { Joints.Add(Ja); }
				}
			}
			Joints.Add(SL0 + SL);
			for (int32 k = 0; k + 1 < Joints.Num(); ++k)
			{
				const FBox2D Panel(FVector2D(Joints[k], Bands[b].X), FVector2D(Joints[k + 1], Bands[b].Y));
				double Dz = 0.0;
				if (Rough > 0.0)
				{
					// Most panels within a few millimetres of their neighbours; about one in eight 4-16 mm off (at 1).
					const bool bBig = GRng.FRand() < 0.12;
					const double Sign = GRng.FRand() < 0.5 ? -1.0 : 1.0;
					const double Big = bBig ? Sign * RammsPCG::RandRange(GRng, 0.4, 1.6) : 0.0;
					const double Small = RammsPCG::RandRange(GRng, -0.22, 0.22);
					Dz = Rough * (Small + Big);
				}
				const TArray<FBox2D> Rs = MinusRampCuts(Sd, Panel);
				const bool bFree = IsItself(Rs, Panel) && IsItself(MinusRampCuts(Sd, Panel.ExpandBy(5.0)), Panel.ExpandBy(5.0));
				for (const FBox2D& Rc : Rs)
				{
					FTile& T = Tiles.AddDefaulted_GetRef();
					T.R = Rc;
					T.Dz = bFree ? Dz : 0.0;
					T.Band = b;
					T.bWhole = bFree;
				}
			}
		}
		// Which whole panels of the route band (out of the corner squares, inside one face of the ground) become a
		// heave, a crack, a utility cover or a patch of pavers.
		const int32 RouteBand = Bands.Num() - 1;
		TArray<int32> Cands;
		for (int32 t = 0; t < Tiles.Num(); ++t)
		{
			const FTile& T = Tiles[t];
			if (!T.bWhole || T.Band != RouteBand) { continue; }
			if (T.R.Min.X < SW + 1.0 || T.R.Max.X > Sd.Length - SW - 1.0) { continue; }
			if (T.R.GetSize().X < PL * 0.6 || T.R.GetSize().Y < 60.0) { continue; }
			if (SidePolys(Ref, T.R).Num() != 1) { continue; }
			Cands.Add(t);
		}
		const double Densities[4] = { S->HeaveDensity, S->CrackDensity, S->CoverDensity, S->PavingPatchDensity };
		for (int32 Kd = 0; Kd < 4; ++Kd)
		{
			if (Densities[Kd] <= 0.0) { continue; }
			const double Jit = RammsPCG::RandRange(GRng, -0.4, 0.4);
			const int32 Want = FMath::Max(0, FMath::RoundToInt(Densities[Kd] * Sd.Length / 10000.0 + Jit));
			for (int32 n = 0; n < Want && Cands.Num() > 0; ++n)
			{
				const int32 c = GRng.RandRange(0, Cands.Num() - 1);
				Tiles[Cands[c]].Special = Kd + 1;
				Cands.RemoveAt(c);
			}
		}

		RammsGround::FPrisms Walk(At(SL0 + SL * 0.5, (Pave0 + SW) * 0.5));
		auto AddPlain = [&](const FBox2D& Rc, double Dz)
		{
			for (const TArray<FVector2D>& Pg : SidePolys(Ref, Rc)) { Walk.AddPoly(Pg, [&](const FVector2D& Q) { return WalkZ(Q) + Dz; }, 0, SurfConcrete); }
		};
		for (const FTile& T : Tiles)
		{
			const FBox2D& Rt = T.R;
			const FVector2D Size2 = Rt.GetSize();
			if (T.Special == 1)
			{
				// A heaved panel: tilted, one edge 1-5 cm proud (its sides taper), the opposite edge flush.
				const double Lip = RammsPCG::RandRange(GRng, 1.0, 5.0);
				const int32 Edge = GRng.RandRange(0, 2);
				RammsGround::FPrisms Hv(At(Rt.GetCenter().X, Rt.GetCenter().Y));
				for (const TArray<FVector2D>& Pg : SidePolys(Ref, Rt))
				{
					Hv.AddPoly(Pg, [&](const FVector2D& Q)
					{
						const FVector2D Lq = ToLocal(Q);
						const double Tt = Edge == 0 ? (Rt.Max.X - Lq.X) / Size2.X : (Edge == 1 ? (Lq.X - Rt.Min.X) / Size2.X : (Lq.Y - Rt.Min.Y) / Size2.Y);
						return WalkZ(Q) + T.Dz + Lip * FMath::Clamp(Tt, 0.0, 1.0);
					}, 0, SurfConcrete);
				}
				WriteFeature(Hv, MatWalk, FName(TEXT("Hazard")), FName(TEXT("Heave")), Lip, VMax, Lip <= VMax, ERammsComplianceIssue::VerticalChangeTooHigh);
			}
			else if (T.Special == 2)
			{
				// A cracked panel: split along a line, one part 2-15 mm proud of the other.
				const double Fu = RammsPCG::RandRange(GRng, 0.3, 0.7);
				const double Fv = RammsPCG::RandRange(GRng, 0.3, 0.7);
				const double Ang = RammsPCG::RandRange(GRng, 0.0, PI);
				const double Lip = RammsPCG::RandRange(GRng, 0.2, 1.5);
				const FVector2D Cl(Rt.Min.X + Size2.X * Fu, Rt.Min.Y + Size2.Y * Fv);
				const FVector2D Nl(-FMath::Sin(Ang), FMath::Cos(Ang));
				TArray<FVector2D> Below, Above;
				RammsGround::SplitByLine(RammsGround::BoxPoly(Rt), Nl, FVector2D::DotProduct(Nl, Cl), Below, Above);
				const FVector Cw3 = At(Cl.X, Cl.Y);
				RammsGround::FPrisms Ck(Cw3);
				if (Below.Num() >= 3) { Ck.AddPoly(LocalPoly(Below), [&](const FVector2D& Q) { return WalkZ(Q) + T.Dz; }, 0, SurfConcrete); }
				if (Above.Num() >= 3) { Ck.AddPoly(LocalPoly(Above), [&](const FVector2D& Q) { return WalkZ(Q) + T.Dz + Lip; }, 0, SurfConcrete); }
				WriteFeature(Ck, MatWalk, FName(TEXT("Surface")), FName(TEXT("Crack")), Lip, VMax, Lip <= VMax, ERammsComplianceIssue::VerticalChangeTooHigh);
			}
			else if (T.Special == 3)
			{
				// A utility cover set into the panel, raised 4-25 mm or sunken 4-30 mm: the panel is cut around it.
				const double Cw0 = RammsPCG::RandRange(GRng, 45.0, 90.0);
				const double Ch0 = RammsPCG::RandRange(GRng, 45.0, 90.0);
				const double Fu = RammsPCG::RandRange(GRng, 0.0, 1.0);
				const double Fv = RammsPCG::RandRange(GRng, 0.0, 1.0);
				const bool bRaised = GRng.FRand() < 0.5;
				const double Mag = bRaised ? RammsPCG::RandRange(GRng, 0.4, 2.5) : RammsPCG::RandRange(GRng, 0.4, 3.0);
				const double Off = bRaised ? Mag : -Mag;
				const double Cw1 = FMath::Min(Cw0, Size2.X - 30.0), Ch1 = FMath::Min(Ch0, Size2.Y - 30.0);
				if (Cw1 < 30.0 || Ch1 < 30.0)
				{
					AddPlain(Rt, T.Dz);
					continue;
				}
				const FVector2D C0(Rt.Min.X + 15.0 + (Size2.X - 30.0 - Cw1) * Fu, Rt.Min.Y + 15.0 + (Size2.Y - 30.0 - Ch1) * Fv);
				const FBox2D Cb(C0, C0 + FVector2D(Cw1, Ch1));
				TArray<FBox2D> Around;
				RammsGround::SubtractBox(Rt, Cb, Around);
				for (const FBox2D& Ar : Around) { AddPlain(Ar, T.Dz); }
				RammsGround::FPrisms Cv(At(Cb.GetCenter().X, Cb.GetCenter().Y));
				for (const TArray<FVector2D>& Pg : SidePolys(Ref, Cb)) { Cv.AddPoly(Pg, [&](const FVector2D& Q) { return WalkZ(Q) + T.Dz + Off; }, 0, SurfMetal); }
				WriteFeature(Cv, MatCover, FName(TEXT("Cover")), bRaised ? FName(TEXT("Raised")) : FName(TEXT("Sunken")), Mag, VMax, Mag <= VMax, ERammsComplianceIssue::VerticalChangeTooHigh);
			}
			else if (T.Special == 4)
			{
				// Pavers: bricks in running bond or granite setts, each a block a few millimetres high or low, their
				// joints 1.5 cm down; at most 1.2 x 1.4 m of the panel, the rest plain.
				const bool bBrick = GRng.FRand() < 0.5;
				const double Su = bBrick ? 20.0 : 12.0, Sv = bBrick ? 10.0 : 12.0, Joint = bBrick ? 0.6 : 1.0, Jit = bBrick ? 0.25 : 0.6;
				const double Sink = 1.5;
				const double Pw = FMath::Min(Size2.X - 10.0, 120.0), Ph = FMath::Min(Size2.Y - 10.0, 140.0);
				const FVector2D P0(Rt.Min.X + (Size2.X - Pw) * 0.5, Rt.Min.Y + (Size2.Y - Ph) * 0.5);
				const FBox2D Pb(P0, P0 + FVector2D(Pw, Ph));
				TArray<FBox2D> Around;
				RammsGround::SubtractBox(Rt, Pb, Around);
				for (const FBox2D& Ar : Around) { AddPlain(Ar, T.Dz); }
				const FName Surf = bBrick ? SurfBrick : SurfCobble;
				RammsGround::FPrisms Pv(At(Pb.GetCenter().X, Pb.GetCenter().Y));
				for (const TArray<FVector2D>& Pg : SidePolys(Ref, Pb)) { Pv.AddPoly(Pg, [&](const FVector2D& Q) { return WalkZ(Q) + T.Dz - Sink; }, 0, Surf); }
				int32 Row = 0;
				for (double Vv = Pb.Min.Y + Joint * 0.5; Vv < Pb.Max.Y - 2.0; Vv += Sv, ++Row)
				{
					const double Shift = (bBrick && (Row % 2) == 1) ? Su * 0.5 : 0.0;
					for (double Uu = Pb.Min.X - Shift + Joint * 0.5; Uu < Pb.Max.X - 2.0; Uu += Su)
					{
						const FBox2D Stone(FVector2D(FMath::Max(Uu, Pb.Min.X + Joint * 0.5), Vv), FVector2D(FMath::Min(Uu + Su - Joint, Pb.Max.X - Joint * 0.5), FMath::Min(Vv + Sv - Joint, Pb.Max.Y - Joint * 0.5)));
						if (Stone.Max.X - Stone.Min.X < 2.0 || Stone.Max.Y - Stone.Min.Y < 2.0) { continue; }
						const double Sdz = RammsPCG::RandRange(GRng, -Jit, Jit);
						Pv.AddPoly(LocalPoly(RammsGround::BoxPoly(Stone)), [&](const FVector2D& Q) { return WalkZ(Q) + T.Dz + Sdz; }, 0, Surf);
					}
				}
				WriteFeature(Pv, bBrick ? MatPaver : MatSett, FName(TEXT("Surface")), bBrick ? FName(TEXT("Brick")) : FName(TEXT("Setts")), Sink, VMax, false, ERammsComplianceIssue::SurfaceUneven);
			}
			else
			{
				AddPlain(Rt, T.Dz);
			}
		}
		if (!Walk.IsEmpty())
		{
			FRammsPieceDesc D = Walk.Desc(20.0);
			D.MaterialPath = MatWalk; D.ManifestSubtype = RammsGround::GroundSubtype; D.Seed = NextGSeed();
			RammsPCG::WritePiece(Pieces, D);
		}
		// Joint lips over VerticalChangeMax between consecutive panels of the route band.
		for (int32 t = 1; t < Tiles.Num(); ++t)
		{
			const FTile& Pt = Tiles[t - 1];
			const FTile& Ct = Tiles[t];
			if (Ct.Band != RouteBand || Pt.Band != RouteBand || !Pt.bWhole || !Ct.bWhole || Pt.Special != 0 || Ct.Special != 0) { continue; }
			if (FMath::Abs(Pt.R.Max.X - Ct.R.Min.X) > 0.5) { continue; }
			const double Lip = FMath::Abs(Ct.Dz - Pt.Dz);
			if (Lip <= VMax) { continue; }
			const int32 f = Features.AddBox(At(Ct.R.Min.X, (Ct.R.Min.Y + Ct.R.Max.Y) * 0.5), Sd.Yaw, FVector(10.0, Ct.R.Max.Y - Ct.R.Min.Y, Lip), NextGSeed());
			RammsPCG::WriteManifest(Features, f, FName(TEXT("Surface")), FName(TEXT("JointLip")), false, ERammsComplianceIssue::VerticalChangeTooHigh, Lip, VMax);
			Features.Set(f, RammsPCG::RouteName, Route);
		}

		// The side for the manifest: the steepest running grade along the route's centre line (out of the corner
		// squares, piece by piece between the ground's bends), its cross slope at mid-side, and its curb heights.
		const double Mc = (FZ + SW) * 0.5;
		const double Mid = Sd.Length * 0.5;
		TArray<double> Stops;
		Stops.Add(FMath::Min(SW + 1.0, Mid));
		{
			TArray<double> Ix, Iy;
			Gm.InnerLines(Bk, Ix, Iy);
			for (const double V : Ix) { if (FMath::Abs(Sd.Dir.X) > 0.5) { Stops.Add((V - Sd.Start.X) / Sd.Dir.X); } }
			for (const double V : Iy) { if (FMath::Abs(Sd.Dir.Y) > 0.5) { Stops.Add((V - Sd.Start.Y) / Sd.Dir.Y); } }
		}
		Stops.Add(FMath::Max(Sd.Length - SW - 1.0, Mid));
		Stops.Sort();
		double Grade = 0.0;
		for (int32 q = 0; q + 1 < Stops.Num(); ++q)
		{
			const double Da = Stops[q + 1] - Stops[q];
			if (Da < 10.0) { continue; }
			Grade = FMath::Max(Grade, FMath::Abs(At(Stops[q + 1], Mc).Z - At(Stops[q], Mc).Z) / Da);
		}
		const double Cross = FMath::Abs(At(Mid, Mc + 10.0).Z - At(Mid, Mc - 10.0).Z) / 20.0;
		const FVector Ctr = At(Mid, Mc);
		{
			const bool bOk = Grade <= S->RampMaxSlope + 1e-4;
			const int32 f = Features.AddBox(Ctr, Sd.Yaw, FVector(Sd.Length, SW, 1.0), NextGSeed());
			RammsPCG::WriteManifest(Features, f, FName(TEXT("Grade")), FName(TEXT("Running")), bOk, bOk ? ERammsComplianceIssue::None : ERammsComplianceIssue::SlopeTooSteep, Grade, S->RampMaxSlope);
			Features.Set(f, RammsPCG::RouteName, Route);
		}
		{
			const bool bOk = Cross <= S->MaxCrossSlope + 1e-4;
			const int32 f = Features.AddBox(Ctr, Sd.Yaw, FVector(Sd.Length, SW, 1.0), NextGSeed());
			RammsPCG::WriteManifest(Features, f, FName(TEXT("Grade")), FName(TEXT("Cross")), bOk, bOk ? ERammsComplianceIssue::None : ERammsComplianceIssue::CrossSlopeTooSteep, Cross, S->MaxCrossSlope);
			Features.Set(f, RammsPCG::RouteName, Route);
		}
		{
			const FVector End = Sd.Start + Sd.Dir * Sd.Length;
			const double R0 = Gm.Reveal(Bk, Sd.Start.X, Sd.Start.Y), R1 = Gm.Reveal(Bk, End.X, End.Y);
			const double Rm = (R0 + R1) * 0.5;
			const int32 f = Features.AddBox(At(Mid, 0.0), Sd.Yaw, FVector(Sd.Length, 20.0, FMath::Max(R0, R1)), NextGSeed());
			RammsPCG::WriteManifest(Features, f, FName(TEXT("Curb")), Rm < 10.0 ? FName(TEXT("Low")) : (Rm > 17.0 ? FName(TEXT("High")) : FName(TEXT("Standard"))), true, ERammsComplianceIssue::None, Rm, 0.0);
			Features.Set(f, RammsPCG::RouteName, Route);
		}
	};
	// A procedural curb run on graded ground, split where the ground bends: its top the walk's edge, tilted along the
	// curb and across it with the walk, reaching 3 cm under the gutter.
	auto EmitGroundCurb = [&](const FSide& Sd, double A0, double A1)
	{
		const int32 Bk = Sd.BlockId;
		TArray<double> Cuts;
		Cuts.Add(A0);
		{
			TArray<double> Ix, Iy;
			Gm.InnerLines(Bk, Ix, Iy);
			for (const double V : Ix) { if (FMath::Abs(Sd.Dir.X) > 0.5) { const double At = (V - Sd.Start.X) / Sd.Dir.X; if (At > A0 + 1.0 && At < A1 - 1.0) { Cuts.Add(At); } } }
			for (const double V : Iy) { if (FMath::Abs(Sd.Dir.Y) > 0.5) { const double At = (V - Sd.Start.Y) / Sd.Dir.Y; if (At > A0 + 1.0 && At < A1 - 1.0) { Cuts.Add(At); } } }
		}
		Cuts.Sort();
		Cuts.Add(A1);
		for (int32 c = 0; c + 1 < Cuts.Num(); ++c)
		{
			const double Ca = Cuts[c], Cb = Cuts[c + 1];
			if (Cb - Ca < 1.0) { continue; }
			const FVector P0 = Sd.Start + Sd.Dir * Ca, P1 = Sd.Start + Sd.Dir * Cb;
			const FVector T0(P0.X, P0.Y, Gm.BlockZ(Bk, P0.X, P0.Y)), T1(P1.X, P1.Y, Gm.BlockZ(Bk, P1.X, P1.Y));
			const FVector Mp = (P0 + P1) * 0.5;
			const FVector Mi = Mp + Sd.Inward * 10.0;
			const double SlopeIn = (Gm.BlockZ(Bk, Mi.X, Mi.Y) - Gm.BlockZ(Bk, Mp.X, Mp.Y)) / 10.0;
			const double Rmax = FMath::Max(Gm.Reveal(Bk, P0.X, P0.Y), Gm.Reveal(Bk, P1.X, P1.Y));
			const FVector XDir = T1 - T0;
			const FVector Nrm = FVector::CrossProduct(XDir, Sd.Inward + FVector(0.0, 0.0, SlopeIn)).GetSafeNormal();
			FRammsPieceDesc P2;
			P2.Kind = ERammsProcPieceKind::Curb; P2.SizeX = XDir.Size(); P2.SizeY = 20.0; P2.Rise = Rmax + 3.0; P2.MaterialPath = MatCurb;
			P2.Transform = FTransform(RammsGround::FromAxes(XDir, Nrm), T0);
			P2.Seed = NextGSeed();
			RammsPCG::WritePiece(Pieces, P2);
		}
	};

	for (const FSide& Sd : Sides)
	{
		const FBlock& B = Blocks[Sd.BlockId];
		// Curb uses (StreetRules), decided before the parking that leaves room for them: a hydrant a few metres
		// from one corner (on about one side in three of a short block, more often on long ones: downtown hydrants
		// stand ~100 m apart), and on a long built side now and then a bus stop zone (a bus length) before its far
		// corner. The hazard placer puts the hydrant and the stop where they are kept.
		double HydrantAt = -1.0, Bus0 = -1.0, Bus1 = -1.0;
		// Right-hand traffic: the lane at this curb runs along the side when the inward normal is to its right.
		// (Always true for the grid's sides, whose Inward is the right of Dir by construction; kept general.)
		const bool bTrafficAlong = (FVector(-Sd.Dir.Y, Sd.Dir.X, 0.0) | Sd.Inward) > 0.0;
		if (S->bStreetRules && !B.bPark && Sd.Length > 1500.0)
		{
			const double FromCorner = CW + 260.0 + RammsPCG::RandRange(Rng, 300.0, 450.0);
			if (Rng.FRand() < FMath::Clamp(Sd.Length / 9000.0, 0.25, 0.85)) { HydrantAt = (Sd.Index % 2) == 0 ? FromCorner : Sd.Length - FromCorner; }
			if (Sd.Length > 4500.0 && Rng.FRand() < 0.35)
			{
				Bus1 = Sd.Length - CW - 260.0 - 200.0;
				Bus0 = Bus1 - 2400.0;
				if (HydrantAt > Bus0 - 500.0 && HydrantAt < Bus1 + 500.0) { Bus0 = Bus1 = -1.0; }
			}
		}
		// Slab: east/west sides shortened by SW at both ends so corners are covered once.
		const bool bShort = (Sd.Index == 1 || Sd.Index == 3);
		const double L0 = bShort ? SW : 0.0;
		const double L = Sd.Length - (bShort ? 2.0 * SW : 0.0);
		const bool bStrip = S->PlantingStrip > 0.5 && FZ > 30.0 && !B.bPark;
		if (L > 1.0)
		{
			// A planting strip street: grass between the curb and the walk (paved where the curb ramps land).
			// Where a curb ramp comes in, the ramp piece is the surface: the walk (and strip) are cut around it.
			const double Pave0 = bStrip ? FZ : 0.0;
			if (bGround)
			{
				// The flat world's pieces' seeds are still drawn, so everything after them keeps its seed.
				const int32 NumWalk = MinusRampCuts(Sd, FBox2D(FVector2D(L0, Pave0), FVector2D(L0 + L, SW))).Num();
				for (int32 q = 0; q < NumWalk; ++q) { NextSeed(); }
				if (bStrip)
				{
					const int32 NumStrip = MinusRampCuts(Sd, FBox2D(FVector2D(L0, 0.0), FVector2D(L0 + L, FZ))).Num();
					for (int32 q = 0; q < NumStrip; ++q) { NextSeed(); }
				}
				EmitGroundSide(Sd, L0, L, Pave0, bStrip, FName(*FString::Printf(TEXT("Block%d_Side%d"), B.Id, Sd.Index)));
			}
			else
			{
				for (const FBox2D& R : MinusRampCuts(Sd, FBox2D(FVector2D(L0, Pave0), FVector2D(L0 + L, SW))))
				{
					const FVector2D Sz = R.GetSize(), Cn = R.GetCenter();
					RammsPCG::WriteCube(Primitives, Sd.Start + Sd.Dir * Cn.X + Sd.Inward * Cn.Y + FVector(0, 0, -10.0), FVector(Sz.X, Sz.Y, 20.0), Sd.Yaw, MatWalk, NextSeed());
				}
			}
			if (bStrip && !bGround)
			{
				for (const FBox2D& R : MinusRampCuts(Sd, FBox2D(FVector2D(L0, 0.0), FVector2D(L0 + L, FZ))))
				{
					FRammsPieceDesc G;
					G.Kind = ERammsProcPieceKind::Slab;
					G.SizeX = R.GetSize().X; G.SizeY = R.GetSize().Y; G.SizeZ = 20.0; G.UVScale = 350.0;
					G.MaterialPath = MatLawn;
					G.Transform = FTransform(FRotator(0.0, Sd.Yaw, 0.0), Sd.Start + Sd.Dir * R.Min.X + Sd.Inward * R.GetCenter().Y + FVector(0, 0, -0.5));
					G.Seed = NextSeed();
					RammsPCG::WritePiece(Pieces, G);
				}
				// (The lots dress the strip in front of them - trees, grass, lights - around their driveways and ramps.)
			}
		}
		// Sidewalk wear: cracks and stains projected onto the walk.
		if (S->DecalDensity > 0.0 && Palette && L > 200.0)
		{
			const int32 N = FMath::RoundToInt(S->DecalDensity * 0.8 * L / 10000.0 + RammsPCG::RandRange(Rng, -0.4, 0.4));
			for (int32 n = 0; n < N; ++n)
			{
				const bool bStain = Rng.FRand() < 0.35;
				const FString M = Palette->PickMaterialPath(bStain ? RammsPaletteSlots::DecalStain : RammsPaletteSlots::DecalWalkCrack, Rng);
				if (M.IsEmpty()) { continue; }
				const double Sz = RammsPCG::RandRange(Rng, 80.0, 200.0);
				const double Lo = bStrip ? FZ + Sz * 0.3 : Sz * 0.3, Hi = SW - Sz * 0.3;
				if (Hi <= Lo) { continue; }
				FRammsPieceDesc Dc;
				Dc.Kind = ERammsProcPieceKind::Decal;
				Dc.SizeX = Sz; Dc.SizeY = Sz * RammsPCG::RandRange(Rng, 0.5, 1.0); Dc.SizeZ = 40.0;
				Dc.MaterialPath = M;
				Dc.Transform = FTransform(FRotator(0.0, RammsPCG::RandRange(Rng, 0.0, 360.0), 0.0), Sd.Start + Sd.Dir * RammsPCG::RandRange(Rng, L0 + 60.0, L0 + L - 60.0) + Sd.Inward * RammsPCG::RandRange(Rng, Lo, Hi));
				if (bGround)
				{
					FVector Loc = Dc.Transform.GetLocation();
					Loc.Z = Gm.BlockZ(Sd.BlockId, Loc.X, Loc.Y);
					Dc.Transform.SetLocation(Loc);
				}
				Dc.Seed = NextSeed();
				RammsPCG::WritePiece(Pieces, Dc);
			}
		}
		// Parked cars at the curb (in the road, outside the curb line), clear of crossings and ramps.
		if (S->ParkingDensity > 0.0 && RW >= 550.0 && Sd.Length > 1500.0)
		{
			FRammsPaletteMesh CarMesh;
			if (Palette && Palette->PickMesh(RammsPaletteSlots::Car, Rng, CarMesh))
			{
				// Each car is picked first, so the spacing and the crossing checks use its own length.
				double a = CW + 260.0;   // rear end of the next slot
				while (true)
				{
					FRammsPaletteMesh Pick;
					if (!Palette->PickMesh(RammsPaletteSlots::Car, Rng, Pick)) { break; }
					const double CarL = FMath::Max(Pick.Footprint.X, 380.0);
					if (a + CarL > Sd.Length - CW - 260.0) { break; }
					const double Gap = RammsPCG::RandRange(Rng, 120.0, 320.0);
					const double c0 = a, c1 = a + CarL;
					a = c1 + Gap;
					if (Rng.FRand() > FMath::Clamp(S->ParkingDensity, 0.0, 1.0)) { continue; }
					bool bSkip = false;
					// With the street rules no car stands within 6 m before a crossing in the direction of travel (20 ft:
					// drivers and the people crossing see each other) or 1.5 m after it, 4.6 m of a hydrant (15 ft), in a
					// bus stop zone or in a kept-clear area; otherwise just clear of the crossings.
					const double GapBefore = S->bStreetRules ? (bTrafficAlong ? 600.0 : 150.0) : 60.0;
					const double GapAfter = S->bStreetRules ? (bTrafficAlong ? 150.0 : 600.0) : 60.0;
					for (const FVector2D& K : Sd.Clear) { if (c1 > K.X - GapBefore && c0 < K.Y + GapAfter) { bSkip = true; break; } }
					if (HydrantAt > 0.0 && c1 > HydrantAt - 460.0 && c0 < HydrantAt + 460.0) { bSkip = true; }
					if (Bus0 >= 0.0 && c1 > Bus0 && c0 < Bus1) { bSkip = true; }
					if (bSkip) { continue; }
					const FVector P = Sd.Start + Sd.Dir * ((c0 + c1) * 0.5) - Sd.Inward * (FMath::Max(Pick.Footprint.Y, 160.0) * 0.5 + 25.0) + FVector(0, 0, Zr - Z0);
					if (InKeep(P, 300.0)) { continue; }
					// Right-hand traffic: a car parked at this curb faces the way its lane runs, the curb on its right.
					// Otherwise either way.
					const double CarYaw = S->bStreetRules ? Sd.Yaw + (bTrafficAlong ? 0.0 : 180.0) : Sd.Yaw + (Rng.FRand() < 0.5 ? 0.0 : 180.0);
					if (bGround)
					{
						// On the graded road, tilted onto it; never on a speed hump (its seed is still drawn).
						const int32 CarSeed = NextSeed();
						bool bOnHump = false;
						for (const FBox2D& Hb : HumpBoxes) { if (Hb.ExpandBy(CarL * 0.5).IsInside(FVector2D(P.X, P.Y))) { bOnHump = true; break; } }
						if (bOnHump) { continue; }
						const int32 c = RammsGround::WritePickedMeshOn(Props, Pick, FVector(P.X, P.Y, Gm.RoadZ(P.X, P.Y)), CarYaw, Gm.Normal(P.X, P.Y), CarSeed);
						RammsPCG::WriteManifest(Props, c, FName(TEXT("Vehicle")), FName(TEXT("Parked")), true, ERammsComplianceIssue::None, 0.0, 0.0);
						continue;
					}
					const int32 c = RammsPCG::WritePickedMesh(Props, Pick, P, CarYaw, NextSeed());
					RammsPCG::WriteManifest(Props, c, FName(TEXT("Vehicle")), FName(TEXT("Parked")), true, ERammsComplianceIssue::None, 0.0, 0.0);
				}
			}
		}
		// Curbs along the curb line, skipping ramp keepouts: one mesh per step when the palette has a curb
		// mesh, otherwise one continuous procedural piece per uninterrupted run.
		if (bGround)
		{
			// Graded: procedural runs (a curb mesh has one height; the curb's varies). The flat world's picks and seeds
			// are still drawn, so the rest of the layout keeps them.
			if (bCurbMesh)
			{
				for (double a = CurbStep * 0.5; a < Sd.Length; a += CurbStep)
				{
					bool bSkip = false;
					for (const FVector2D& K : Sd.Keepouts) { if (a > K.X - CurbStep * 0.5 && a < K.Y + CurbStep * 0.5) { bSkip = true; break; } }
					if (bSkip) { continue; }
					FRammsPaletteMesh Burn;
					Palette->PickMesh(RammsPaletteSlots::Curb, Rng, Burn);
					NextSeed();
				}
			}
			TArray<FVector2D> Cuts = Sd.Keepouts;
			Cuts.Sort([](const FVector2D& Ka, const FVector2D& Kb) { return Ka.X < Kb.X; });
			double RunStart = 0.0;
			auto Run = [&](double A0, double A1)
			{
				if (A1 - A0 < 20.0) { return; }
				if (!bCurbMesh) { NextSeed(); }
				EmitGroundCurb(Sd, A0, A1);
			};
			for (const FVector2D& K : Cuts)
			{
				Run(RunStart, FMath::Clamp(K.X, RunStart, Sd.Length));
				RunStart = FMath::Max(RunStart, FMath::Min(K.Y, Sd.Length));
			}
			Run(RunStart, Sd.Length);
		}
		else if (bCurbMesh)
		{
			for (double a = CurbStep * 0.5; a < Sd.Length; a += CurbStep)
			{
				bool bSkip = false;
				for (const FVector2D& K : Sd.Keepouts) { if (a > K.X - CurbStep * 0.5 && a < K.Y + CurbStep * 0.5) { bSkip = true; break; } }
				if (bSkip) { continue; }
				RammsPCG::WritePaletteMesh(Props, Palette, RammsPaletteSlots::Curb, Rng, Sd.Start + Sd.Dir * a + FVector(0, 0, -CH), Sd.Yaw, NextSeed());
			}
		}
		else
		{
			TArray<FVector2D> Cuts = Sd.Keepouts;
			Cuts.Sort([](const FVector2D& A, const FVector2D& B) { return A.X < B.X; });
			double RunStart = 0.0;
			auto EmitRun = [&](double A0, double A1)
			{
				if (A1 - A0 < 20.0) { return; }
				FRammsPieceDesc P2;
				P2.Kind = ERammsProcPieceKind::Curb; P2.SizeX = A1 - A0; P2.SizeY = 20.0; P2.Rise = CH; P2.MaterialPath = MatCurb; P2.Seed = NextSeed();
				P2.Transform = FTransform(FRotator(0.0, Sd.Yaw, 0.0), Sd.Start + Sd.Dir * A0);
				RammsPCG::WritePiece(Pieces, P2);
			};
			for (const FVector2D& K : Cuts)
			{
				EmitRun(RunStart, FMath::Clamp(K.X, RunStart, Sd.Length));
				RunStart = FMath::Max(RunStart, FMath::Min(K.Y, Sd.Length));
			}
			EmitRun(RunStart, Sd.Length);
		}
		// Side point for the hazard placer.
		const int32 s = SidewalksOut.Add(FTransform(FRotator(0.0, Sd.Yaw, 0.0), Sd.Start), FVector(0.0, 0.0, 0.0), FVector(Sd.Length, SW, 0.0), NextSeed());
		SidewalksOut.SetInt(s, RammsPCG::BlockId, B.Id);
		SidewalksOut.SetInt(s, RammsPCG::Side, Sd.Index);
		SidewalksOut.Set(s, RammsPCG::Length, Sd.Length);
		SidewalksOut.Set(s, RammsPCG::Width, SW);
		SidewalksOut.Set(s, RammsPCG::FurnitureZone, FZ);
		SidewalksOut.Set(s, RammsPCG::RouteWidth, SW - FZ);
		SidewalksOut.SetBool(s, RammsPCG::IsPark, B.bPark);
		FString Keep;
		for (const FVector2D& K : Sd.Clear) { Keep += FString::Printf(TEXT("%.0f:%.0f;"), K.X, K.Y); }
		SidewalksOut.Set(s, FName(TEXT("Keepouts")), Keep);
		// What the hazard placer needs of the road beside the side: its width and how far below the walk it is
		// (litter in the gutter, manholes in the lane), and the curb uses kept for it.
		SidewalksOut.Set(s, FName(TEXT("RoadWidth")), RW);
		SidewalksOut.Set(s, FName(TEXT("RoadDrop")), Z0 - Zr);
		if (HydrantAt > 0.0) { SidewalksOut.Set(s, FName(TEXT("HydrantAt")), HydrantAt); }
		if (Bus0 >= 0.0) { SidewalksOut.Set(s, FName(TEXT("BusZone")), FString::Printf(TEXT("%.0f:%.0f"), Bus0, Bus1)); }
		// The intersection crossings (not mid-block ones), for the hazard placer's corner signals.
		if (Sd.CornerCrossings.Num() > 0)
		{
			FString Corners;
			for (const FVector2D& K : Sd.CornerCrossings) { Corners += FString::Printf(TEXT("%.0f:%.0f;"), K.X, K.Y); }
			SidewalksOut.Set(s, FName(TEXT("CornerCrossings")), Corners);
		}
		SidewalksOut.Set(s, RammsPCG::RouteName, FName(*FString::Printf(TEXT("Block%d_Side%d"), B.Id, Sd.Index)));
		if (bGround) { SidewalksOut.Set(s, RammsGround::Attr, GroundText); }
	}
	for (const FBlock& B : Blocks)
	{
		const double lx0 = B.X0 + RW * 0.5 + SW, lx1 = B.X1 - RW * 0.5 - SW, ly0 = B.Y0 + RW * 0.5 + SW, ly1 = B.Y1 - RW * 0.5 - SW;
		const FVector LotCentre((lx0 + lx1) * 0.5, (ly0 + ly1) * 0.5, Z0);
		const FVector LotSize(lx1 - lx0, ly1 - ly0, 0.0);
		const int32 b = BlocksOut.Add(FTransform(LotCentre), -LotSize * 0.5, LotSize * 0.5, RammsPCG::SubSeed(Seed, 1000 + B.Id));
		BlocksOut.SetInt(b, RammsPCG::BlockId, B.Id);
		BlocksOut.SetBool(b, RammsPCG::IsPark, B.bPark);
		BlocksOut.Set(b, RammsPCG::Width, LotSize.X);
		BlocksOut.Set(b, RammsPCG::Depth, LotSize.Y);
		// The block's curb ramps (world XY boxes), so the lots keep their strip trees, lights and driveway
		// aprons off them.
		{
			const FBox2D Outer(FVector2D(B.X0, B.Y0), FVector2D(B.X1, B.Y1));
			FString Cuts;
			for (const FBox2D& Cw : RampCuts)
			{
				if (RammsPCG::Overlaps(Cw, Outer)) { Cuts += FString::Printf(TEXT("%.0f,%.0f,%.0f,%.0f;"), Cw.Min.X, Cw.Min.Y, Cw.Max.X, Cw.Max.Y); }
			}
			BlocksOut.Set(b, FName(TEXT("RampCuts")), Cuts);
		}
		if (bGround) { BlocksOut.Set(b, RammsGround::Attr, GroundText); }
		// Ground inside the block: lawn for parks and yards, concrete for built blocks (buildings sit on
		// it). A slab piece rather than a scaled cube so the material tiles at a sensible size.
		if (bGround)
		{
			// Graded: level with the sidewalk's back edge, one plane per street-grid cell of the block.
			TArray<TArray<FVector2D>> Polys = { RammsGround::BoxPoly(FBox2D(FVector2D(lx0, ly0), FVector2D(lx1, ly1))) };
			TArray<double> Ix, Iy;
			Gm.InnerLines(B.Id, Ix, Iy);
			for (const double V : Ix) { RammsGround::SplitAllByLine(Polys, FVector2D(1.0, 0.0), V); }
			for (const double V : Iy) { RammsGround::SplitAllByLine(Polys, FVector2D(0.0, 1.0), V); }
			RammsGround::FPrisms Lot(FVector(LotCentre.X, LotCentre.Y, Gm.BlockZ(B.Id, LotCentre.X, LotCentre.Y)));
			for (const TArray<FVector2D>& Pg : Polys)
			{
				Lot.AddPoly(Pg, [&](const FVector2D& Q) { return Gm.BlockZ(B.Id, Q.X, Q.Y); }, 0, B.bLawn ? SurfGrass : SurfConcrete);
			}
			FRammsPieceDesc G = Lot.Desc(20.0);
			G.UVScale = B.bLawn ? 350.0 : 250.0;
			G.MaterialPath = B.bLawn ? MatLawn : MatConcrete;
			G.ManifestSubtype = RammsGround::GroundSubtype;
			G.Seed = NextSeed();
			RammsPCG::WritePiece(Pieces, G);
		}
		else
		{
			FRammsPieceDesc G;
			G.Kind = ERammsProcPieceKind::Slab;
			G.SizeX = LotSize.X; G.SizeY = LotSize.Y; G.SizeZ = 20.0;
			G.UVScale = B.bLawn ? 350.0 : 250.0;
			G.MaterialPath = B.bLawn ? MatLawn : MatConcrete;
			G.Transform = FTransform(FRotator::ZeroRotator, FVector(lx0, LotCentre.Y, Z0)); // slab top sits at its pivot
			G.Seed = NextSeed();
			RammsPCG::WritePiece(Pieces, G);
		}
	}

	// --- traffic records (ramms_worldgen.traffic lays the vehicle lanes from them) ----------------------------------
	// Records only, appended after everything else (no Rng draws, own seeds): nothing placed changes. One Road per
	// live segment: its centre on the centreline at road level, yaw 0 along X / 90 along Y, size.x the length between
	// the intersection squares, size.y the road width (also Measure). One Intersection per node a live segment
	// touches: the square at road level, Measure the number of segments meeting there, subtype Cross (4), Tee (3),
	// Through or Bend (2), End (1).
	{
		int32 TrafficRecord = 0;
		for (const FSeg& Sg : Segs)
		{
			if (!Sg.bLive) { continue; }
			const double SegA = Sg.bAlongX ? X[Sg.I] : Y[Sg.J];
			const double SegB = Sg.bAlongX ? X[Sg.I + 1] : Y[Sg.J + 1];
			const double SegAcross = Sg.bAlongX ? Y[Sg.J] : X[Sg.I];
			const FVector RoadCentre = Sg.bAlongX ? FVector((SegA + SegB) * 0.5, SegAcross, Zr) : FVector(SegAcross, (SegA + SegB) * 0.5, Zr);
			const FVector RoadAt(RoadCentre.X, RoadCentre.Y, bGround ? Gm.RoadZ(RoadCentre.X, RoadCentre.Y) : Zr); // graded: the crown's height
			const int32 f = Features.AddBox(RoadAt, Sg.bAlongX ? 0.0 : 90.0, FVector(SegB - SegA - RW, RW, 1.0), RammsPCG::SubSeed(Seed, 900000 + TrafficRecord++));
			RammsPCG::WriteManifest(Features, f, FName(TEXT("Road")), Sg.bAlongX ? FName(TEXT("AlongX")) : FName(TEXT("AlongY")), true, ERammsComplianceIssue::None, RW, 0.0);
		}
		for (int32 j = 0; j < NY; ++j)
		{
			for (int32 i = 0; i < NX; ++i)
			{
				const bool bWest = i > 0 && Segs[SegIndexX(i - 1, j)].bLive;
				const bool bEast = i + 1 < NX && Segs[SegIndexX(i, j)].bLive;
				const bool bSouth = j > 0 && Segs[SegIndexY(i, j - 1)].bLive;
				const bool bNorth = j + 1 < NY && Segs[SegIndexY(i, j)].bLive;
				const int32 Arms = (bWest ? 1 : 0) + (bEast ? 1 : 0) + (bSouth ? 1 : 0) + (bNorth ? 1 : 0);
				if (Arms == 0) { continue; }
				const TCHAR* Kind = Arms >= 4 ? TEXT("Cross") : Arms == 3 ? TEXT("Tee") : Arms == 1 ? TEXT("End") : ((bWest && bEast) || (bSouth && bNorth)) ? TEXT("Through") : TEXT("Bend");
				const int32 f = Features.AddBox(FVector(X[i], Y[j], bGround ? Gm.RoadZ(X[i], Y[j]) : Zr), 0.0, FVector(RW, RW, 1.0), RammsPCG::SubSeed(Seed, 900000 + TrafficRecord++));
				RammsPCG::WriteManifest(Features, f, FName(TEXT("Intersection")), FName(Kind), true, ERammsComplianceIssue::None, Arms, 0.0);
			}
		}
	}
	// --- end traffic records ------------------------------------------------------------------------------------------
	// Graded streets stand above the generator's height: grassy banks fall from the site's outer gutters, at 1:3 at the
	// steepest, to 50 cm under it (the example levels' ground is 45 cm under it), so the site has no cliff edge.
	if (bGround && S->MaxStreetGrade > 0.0)
	{
		const double Hh = RW * 0.5;
		const double Foot = Z0 - 50.0;
		auto Gutter = [&](double Gx, double Gy) { return Gm.Base(Gx, Gy) - CH; };
		RammsGround::FPrisms Banks(FVector(Centre.X, Centre.Y, Foot));
		auto Tri = [&](const FVector& Ta, const FVector& Tb, const FVector& Tc) { Banks.Add({ Ta, Tb, Tc }, 0, SurfGrass); };
		// Along each edge: the outer gutter line (its height bends at the street lines) and a straight foot line.
		const double Xa = X[0] - Hh, Xb = X.Last() + Hh, Ya = Y[0] - Hh, Yb = Y.Last() + Hh;
		TArray<double> Xs = { Xa }, Ys = { Ya };
		for (const double V : X) { Xs.Add(V); }
		for (const double V : Y) { Ys.Add(V); }
		Xs.Add(Xb);
		Ys.Add(Yb);
		double DepthS = 0.0, DepthN = 0.0, DepthW = 0.0, DepthE = 0.0;
		for (const double V : Xs) { DepthS = FMath::Max(DepthS, Gutter(V, Ya) - Foot); DepthN = FMath::Max(DepthN, Gutter(V, Yb) - Foot); }
		for (const double V : Ys) { DepthW = FMath::Max(DepthW, Gutter(Xa, V) - Foot); DepthE = FMath::Max(DepthE, Gutter(Xb, V) - Foot); }
		const double Ws = DepthS * 3.0, Wn = DepthN * 3.0, Ww = DepthW * 3.0, We = DepthE * 3.0;
		for (int32 k = 0; k + 1 < Xs.Num(); ++k)
		{
			const double U0 = Xs[k], U1 = Xs[k + 1];
			if (U1 - U0 < 1.0) { continue; }
			// south and north banks
			Tri(FVector(U0, Ya, Gutter(U0, Ya)), FVector(U1, Ya - Ws, Foot), FVector(U0, Ya - Ws, Foot));
			Tri(FVector(U0, Ya, Gutter(U0, Ya)), FVector(U1, Ya, Gutter(U1, Ya)), FVector(U1, Ya - Ws, Foot));
			Tri(FVector(U0, Yb, Gutter(U0, Yb)), FVector(U0, Yb + Wn, Foot), FVector(U1, Yb + Wn, Foot));
			Tri(FVector(U0, Yb, Gutter(U0, Yb)), FVector(U1, Yb + Wn, Foot), FVector(U1, Yb, Gutter(U1, Yb)));
		}
		for (int32 k = 0; k + 1 < Ys.Num(); ++k)
		{
			const double V0 = Ys[k], V1 = Ys[k + 1];
			if (V1 - V0 < 1.0) { continue; }
			// west and east banks
			Tri(FVector(Xa, V0, Gutter(Xa, V0)), FVector(Xa - Ww, V0, Foot), FVector(Xa - Ww, V1, Foot));
			Tri(FVector(Xa, V0, Gutter(Xa, V0)), FVector(Xa - Ww, V1, Foot), FVector(Xa, V1, Gutter(Xa, V1)));
			Tri(FVector(Xb, V0, Gutter(Xb, V0)), FVector(Xb + We, V1, Foot), FVector(Xb + We, V0, Foot));
			Tri(FVector(Xb, V0, Gutter(Xb, V0)), FVector(Xb, V1, Gutter(Xb, V1)), FVector(Xb + We, V1, Foot));
		}
		// The four corners: from the site's corner down to both banks' feet, and the level foot beyond.
		const double Cx[4] = { Xa, Xb, Xb, Xa }, Cy[4] = { Ya, Ya, Yb, Yb };
		const double Dx[4] = { -Ww, We, We, -Ww }, Dy[4] = { -Ws, -Ws, Wn, Wn };
		for (int32 c = 0; c < 4; ++c)
		{
			const FVector In(Cx[c], Cy[c], Gutter(Cx[c], Cy[c]));
			const FVector Fx(Cx[c] + Dx[c], Cy[c], Foot), Fy(Cx[c], Cy[c] + Dy[c], Foot), Fo(Cx[c] + Dx[c], Cy[c] + Dy[c], Foot);
			Tri(In, Fx, Fy);
			Tri(Fx, Fo, Fy);
		}
		if (!Banks.IsEmpty())
		{
			FRammsPieceDesc D = Banks.Desc(20.0);
			D.MaterialPath = MatLawn;
			D.UVScale = 400.0;
			D.ManifestSubtype = RammsGround::GroundSubtype;
			D.Seed = NextGSeed();
			RammsPCG::WritePiece(Pieces, D);
		}
	}

	Primitives.Emit(Context, RammsPCG::PinPrimitives, { TEXT("Street") });
	Props.Emit(Context, RammsPCG::PinProps, { TEXT("Curb") });
	Pieces.Emit(Context, RammsPCG::PinPieces, { TEXT("Street") });
	BlocksOut.Emit(Context, RammsPCG::PinBlocks);
	SidewalksOut.Emit(Context, RammsPCG::PinSidewalks);
	Features.Emit(Context, RammsPCG::PinFeatures);
	return true;
}

#undef LOCTEXT_NAMESPACE
