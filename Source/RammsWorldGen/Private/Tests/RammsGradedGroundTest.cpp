// Copyright ATDev. All Rights Reserved.

// Graded ground. The ground model's surfaces meet without a lip anywhere (road halves, intersection triangles, walks,
// corner zones, the lots' ring), the curb's top is the walk's edge and the gutter plus the curb height, a walk's cross
// slope is exactly its block's setting whatever the hill does and its running slope its curb's, the text round-trips
// exactly and every facet a polygon is cut into is planar. A graded world generated from a preset keeps its curb ramps'
// feet on the road (no slot), its entrance steps and ramps on the ground, its speed humps' and cracks' records on their
// pieces, its walks' cross slopes within the settings, every lip over VerticalChangeMax along its routes recorded, and
// is the same twice from one seed.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CollisionQueryParams.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Math/RandomStream.h"
#include "Misc/Crc.h"
#include "PCG/RammsGround.h"
#include "RammsProcPiece.h"
#include "RammsWorldGenerator.h"
#include "RammsWorldPreset.h"

namespace RammsGradedGroundTest
{
	// A 3 x 3 street grid with the south row's two cells merged (the street along Y at x = 5000 dropped there), grades of
	// either sign along both axes, a crown, curbs varying along their sides and three cross slopes.
	FRammsGroundModel MakeModel()
	{
		FRammsGroundModel M;
		M.Z0 = 1000.0;
		M.RoadWidth = 800.0;
		M.SidewalkWidth = 360.0;
		M.CurbHeight = 15.0;
		M.Crown = 0.02;
		M.X = { 0.0, 5000.0, 10000.0 };
		M.Y = { 0.0, 4500.0, 9000.0 };
		M.FX = { 0.0, 250.0, 60.0 };   // +5 %, then -3.8 %
		M.FY = { 120.0, 0.0, 330.0 };  // -2.7 %, then +7.3 %
		M.Arms.SetNumZeroed(9);
		for (int32 j = 0; j < 3; ++j)
		{
			for (int32 i = 0; i < 3; ++i)
			{
				uint8 A = 0;
				if (i < 2) { A |= FRammsGroundModel::ArmE; }
				if (i > 0) { A |= FRammsGroundModel::ArmW; }
				if (j < 2 && !(i == 1 && j == 0)) { A |= FRammsGroundModel::ArmN; }
				if (j > 0 && !(i == 1 && j == 1)) { A |= FRammsGroundModel::ArmS; }
				M.Arms[j * 3 + i] = A;
			}
		}
		auto AddBlock = [&M](double X0, double X1, double Y0, double Y1, double Reveal, double Rx, double Ry, double Cross)
		{
			FRammsGroundModel::FBlock& B = M.Blocks.AddDefaulted_GetRef();
			B.X0 = X0; B.X1 = X1; B.Y0 = Y0; B.Y1 = Y1;
			B.Reveal = Reveal; B.RevealX = Rx; B.RevealY = Ry;
			B.CrossSlope = Cross;
		};
		AddBlock(0.0, 10000.0, 0.0, 4500.0, 12.0, 0.0006, -0.0010, 0.020);
		AddBlock(0.0, 5000.0, 4500.0, 9000.0, 16.0, -0.0012, 0.0008, 0.012);
		AddBlock(5000.0, 10000.0, 4500.0, 9000.0, 9.0, 0.0010, 0.0012, 0.045);
		M.Build();
		return M;
	}

	// The Street Network's sides of a block: 0 south, 1 east, 2 north, 3 west, from their start corner along Dir with the
	// block on their left (Inward).
	struct FWalkSide { FVector2D Start, Dir, Inward; double Length = 0.0; };
	FWalkSide SideOf(const FRammsGroundModel& M, int32 Block, int32 Side)
	{
		const FBox2D R = M.CurbRect(Block);
		const FVector2D Cn[4] = { R.Min, FVector2D(R.Max.X, R.Min.Y), R.Max, FVector2D(R.Min.X, R.Max.Y) };
		const FVector2D Dir[4] = { FVector2D(1.0, 0.0), FVector2D(0.0, 1.0), FVector2D(-1.0, 0.0), FVector2D(0.0, -1.0) };
		FWalkSide S;
		S.Start = Cn[Side];
		S.Dir = Dir[Side];
		S.Inward = FVector2D(-S.Dir.Y, S.Dir.X);
		S.Length = (Side % 2 == 0) ? R.GetSize().X : R.GetSize().Y;
		return S;
	}

	double PolyArea(const TArray<FVector2D>& P) { return FMath::Abs(RammsGround::SignedArea(P)); }

	bool StrictlyInside(const TArray<FVector2D>& Poly, const FVector2D& P, double Margin)
	{
		for (int32 k = 0; k < Poly.Num(); ++k)
		{
			const FVector2D& A = Poly[k];
			const FVector2D E = Poly[(k + 1) % Poly.Num()] - A;
			const double Len = E.Size();
			if (Len < 1e-9) { continue; }
			if (E.X * (P.Y - A.Y) - E.Y * (P.X - A.X) < Margin * Len) { return false; }
		}
		return true;
	}

	/** The world box of a manifest feature (yaw only), grown by Margin, holds P in plan. */
	bool InFeature(const FRammsManifestFeature& F, const FVector& P, double Margin)
	{
		const FVector D = F.Rotation.UnrotateVector(P - F.Location);
		return FMath::Abs(D.X) <= F.Size.X * 0.5 + Margin && FMath::Abs(D.Y) <= F.Size.Y * 0.5 + Margin;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsGradedGroundModelTest, "Ramms.WorldGen.Graded.GroundModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRammsGradedGroundModelTest::RunTest(const FString& Parameters)
{
	using namespace RammsGradedGroundTest;
	const FRammsGroundModel M = MakeModel();
	TestEqual(TEXT("every block has its facets"), M.Faces.Num(), M.Blocks.Num());
	const double S = M.SidewalkWidth;
	FRandomStream Rs(20261115);

	for (int32 b = 0; b < M.Blocks.Num(); ++b)
	{
		const TArray<FRammsGroundModel::FFace>& Fs = M.Faces[b];
		const FBox2D R = M.CurbRect(b);
		TestTrue(*FString::Printf(TEXT("block %d: has facets"), b), Fs.Num() > 0);

		// The facets tile the curb rectangle: counter-clockwise convex polygons, their areas summing to its area, and a
		// point well inside one is inside no other.
		double Area = 0.0;
		bool bConvex = true;
		for (const FRammsGroundModel::FFace& F : Fs)
		{
			Area += PolyArea(F.Poly);
			for (int32 k = 0; k < F.Poly.Num(); ++k)
			{
				const FVector2D A = F.Poly[k], B = F.Poly[(k + 1) % F.Poly.Num()], C = F.Poly[(k + 2) % F.Poly.Num()];
				const FVector2D E1 = B - A, E2 = C - B;
				bConvex &= (E1.X * E2.Y - E1.Y * E2.X) >= -1e-6 * E1.Size() * E2.Size();
			}
		}
		TestTrue(*FString::Printf(TEXT("block %d: facets are convex and counter-clockwise"), b), bConvex);
		TestTrue(*FString::Printf(TEXT("block %d: facets cover the curb rectangle (%.1f of %.1f cm2)"), b, Area, R.GetArea()), FMath::Abs(Area - R.GetArea()) <= R.GetArea() * 1e-7 + 1.0);
		int32 Overlaps = 0;
		for (int32 n = 0; n < 2000; ++n)
		{
			const FVector2D P(FMath::Lerp(R.Min.X, R.Max.X, Rs.FRand()), FMath::Lerp(R.Min.Y, R.Max.Y, Rs.FRand()));
			int32 In = 0;
			for (const FRammsGroundModel::FFace& F : Fs) { In += StrictlyInside(F.Poly, P, 0.01) ? 1 : 0; }
			Overlaps += In > 1 ? 1 : 0;
		}
		TestEqual(*FString::Printf(TEXT("block %d: no two facets overlap"), b), Overlaps, 0);

		// No lip anywhere: on every facet edge inside the block, this facet's plane and the ground just across the edge
		// agree (within the slope's change over a tenth of a millimetre).
		double WorstLip = 0.0;
		FVector2D WorstAt = FVector2D::ZeroVector;
		for (const FRammsGroundModel::FFace& F : Fs)
		{
			for (int32 k = 0; k < F.Poly.Num(); ++k)
			{
				const FVector2D A = F.Poly[k], B = F.Poly[(k + 1) % F.Poly.Num()];
				const FVector2D E = B - A;
				if (E.Size() < 1.0) { continue; }
				const FVector2D Out = FVector2D(E.Y, -E.X) / E.Size();
				for (const double T : { 0.1, 0.5, 0.9 })
				{
					const FVector2D P = A + E * T;
					const FVector2D Q = P + Out * 0.01;
					if (Q.X <= R.Min.X + 0.02 || Q.X >= R.Max.X - 0.02 || Q.Y <= R.Min.Y + 0.02 || Q.Y >= R.Max.Y - 0.02) { continue; }
					const double Lip = FMath::Abs(F.At(P.X, P.Y) - M.BlockZ(b, Q.X, Q.Y));
					if (Lip > WorstLip) { WorstLip = Lip; WorstAt = P; }
				}
			}
		}
		TestTrue(*FString::Printf(TEXT("block %d: continuous across every facet edge (worst %.4f cm at %s)"), b, WorstLip, *WorstAt.ToString()), WorstLip <= 0.01);

		// Every walk: its curb's top is its edge, the gutter plus the curb height; its cross slope is the block's exactly;
		// its running slope is its curb's.
		for (int32 Sd = 0; Sd < 4; ++Sd)
		{
			const FWalkSide Side = SideOf(M, b, Sd);
			double T0 = 0.0, T1 = 0.0;
			TestTrue(TEXT("a straight range"), M.StraightRange(b, Sd, T0, T1));
			TestTrue(*FString::Printf(TEXT("block %d side %d: a straight part between its corner zones"), b, Sd), T1 > T0 + 100.0);
			for (const double F : { 0.27, 0.5, 0.73 })
			{
				const double T = FMath::Lerp(T0, T1, F);
				auto At = [&](double Tt, double Bb) { const FVector2D Q = Side.Start + Side.Dir * Tt + Side.Inward * Bb; return M.BlockZ(b, Q.X, Q.Y); };
				const FVector2D C = Side.Start + Side.Dir * T;
				const double Edge = At(T, 0.0);
				TestTrue(*FString::Printf(TEXT("block %d side %d: the walk's edge is the curb's top"), b, Sd), FMath::Abs(Edge - M.CurbTop(b, C.X, C.Y)) < 1e-6);
				const double Gutter = M.Base(C.X, C.Y) - M.CurbHeight;
				TestTrue(*FString::Printf(TEXT("block %d side %d: the curb's top is the gutter plus the curb height"), b, Sd), FMath::Abs(Edge - Gutter - M.Reveal(b, C.X, C.Y)) < 1e-6);
				TestTrue(*FString::Printf(TEXT("block %d side %d: the curb is at least 2 cm"), b, Sd), M.Reveal(b, C.X, C.Y) >= 2.0);
				const FVector2D Road = C - Side.Inward * 0.001;
				TestTrue(*FString::Printf(TEXT("block %d side %d: the road meets the curb at the gutter"), b, Sd), FMath::Abs(M.RoadZ(Road.X, Road.Y) - Gutter) < 0.01);
				const double Cross = (At(T, 0.8 * S) - At(T, 0.2 * S)) / (0.6 * S);
				TestTrue(*FString::Printf(TEXT("block %d side %d: cross slope %.6f is the block's %.6f"), b, Sd, Cross, M.Blocks[b].CrossSlope), FMath::Abs(Cross - M.Blocks[b].CrossSlope) < 1e-9);
			}
			// Along the walk, inside one stretch (a corner arm's end to 100 cm on): the curb's grade.
			const double Ta = T0 + 10.0, Tb = T0 + 110.0;
			const FVector2D Ca = Side.Start + Side.Dir * Ta, Cb = Side.Start + Side.Dir * Tb;
			const FVector2D Wa = Ca + Side.Inward * (0.5 * S), Wb = Cb + Side.Inward * (0.5 * S);
			const double Walk = (M.BlockZ(b, Wb.X, Wb.Y) - M.BlockZ(b, Wa.X, Wa.Y)) / 100.0;
			const double Curb = (M.CurbTop(b, Cb.X, Cb.Y) - M.CurbTop(b, Ca.X, Ca.Y)) / 100.0;
			TestTrue(*FString::Printf(TEXT("block %d side %d: running slope %.6f is the curb's %.6f"), b, Sd, Walk, Curb), FMath::Abs(Walk - Curb) < 1e-9);
		}
	}

	// Roads: continuous across the crown, between a segment and its intersection squares, and across the squares'
	// diagonals and half-edges, and at the gutter where it meets each curb (checked above).
	{
		double Worst = 0.0;
		const double H = M.RoadWidth * 0.5;
		auto Jump = [&](const FVector2D& P, const FVector2D& D)
		{
			Worst = FMath::Max(Worst, FMath::Abs(M.RoadZ(P.X + D.X, P.Y + D.Y) - M.RoadZ(P.X - D.X, P.Y - D.Y)));
		};
		for (int32 j = 0; j < M.Y.Num(); ++j)
		{
			for (int32 i = 0; i < M.X.Num(); ++i)
			{
				const FVector2D N(M.X[i], M.Y[j]);
				for (const double F : { 0.25, 0.5, 0.75 })
				{
					Jump(N + FVector2D(H * F, H * F), FVector2D(0.005, -0.005));      // a diagonal
					Jump(N + FVector2D(-H * F, H * F), FVector2D(0.005, 0.005));
					Jump(N + FVector2D(H * F, 0.0), FVector2D(0.0, 0.005));          // a half-edge (the crown line)
					Jump(N + FVector2D(0.0, H * F), FVector2D(0.005, 0.0));
					Jump(N + FVector2D(H, H * F * 0.9), FVector2D(0.005, 0.0));      // the square's edge onto a segment
					Jump(N + FVector2D(H * F * 0.9, H), FVector2D(0.0, 0.005));
				}
				if (i + 1 < M.X.Num()) { Jump(FVector2D((M.X[i] + M.X[i + 1]) * 0.5, M.Y[j]), FVector2D(0.0, 0.005)); }
				if (j + 1 < M.Y.Num()) { Jump(FVector2D(M.X[i], (M.Y[j] + M.Y[j + 1]) * 0.5), FVector2D(0.005, 0.0)); }
			}
		}
		TestTrue(*FString::Printf(TEXT("roads: continuous across crowns, squares and diagonals (worst %.5f cm over 0.1 mm)"), Worst), Worst < 0.002);
	}

	// The text: exact both ways (17 digits), a second parse builds the same facets, and a text that does not parse
	// leaves a model as it was.
	{
		const FString Text = M.ToString();
		FRammsGroundModel M2;
		TestTrue(TEXT("the text parses"), M2.FromString(Text));
		TestEqual(TEXT("the text round-trips exactly"), M2.ToString(), Text);
		bool bSame = M2.Faces.Num() == M.Faces.Num();
		for (int32 b = 0; bSame && b < M.Faces.Num(); ++b) { bSame = M2.Faces[b].Num() == M.Faces[b].Num(); }
		TestTrue(TEXT("the same facets"), bSame);
		double Worst = 0.0;
		for (int32 n = 0; n < 500; ++n)
		{
			const double Px = FMath::Lerp(-500.0, 10500.0, Rs.FRand()), Py = FMath::Lerp(-500.0, 9500.0, Rs.FRand());
			Worst = FMath::Max(Worst, FMath::Abs(M.GroundZ(Px, Py) - M2.GroundZ(Px, Py)));
		}
		TestTrue(*FString::Printf(TEXT("the same heights everywhere (worst %g)"), Worst), Worst == 0.0);
		FRammsGroundModel M3 = M2;
		TestFalse(TEXT("garbage does not parse"), M3.FromString(TEXT("G2|1,2|nonsense")));
		TestEqual(TEXT("a text that does not parse leaves the model alone"), M3.ToString(), Text);
	}

	// Cutting polygons into facets: the parts tile the polygon and each is planar, across a corner zone and across the
	// street dropped inside the merged block.
	{
		const FBox2D R = M.CurbRect(0);
		const TArray<FBox2D> Tests = {
			FBox2D(R.Min + FVector2D(0.0, 0.0), R.Min + FVector2D(1400.0, S)),         // the south-west corner zone and on
			FBox2D(FVector2D(4700.0, R.Min.Y + 100.0), FVector2D(5300.0, R.Min.Y + 300.0)), // across the dropped street
			FBox2D(R.Min + FVector2D(200.0, 200.0), R.Min + FVector2D(1500.0, 1500.0)),  // the corner, the lot's ring
		};
		for (int32 t = 0; t < Tests.Num(); ++t)
		{
			const TArray<FVector2D> Poly = RammsGround::BoxPoly(Tests[t]);
			TArray<TArray<FVector2D>> Parts;
			M.ClipToFaces(0, Poly, Parts);
			double Sum = 0.0, WorstOff = 0.0;
			for (const TArray<FVector2D>& Pc : Parts)
			{
				Sum += PolyArea(Pc);
				if (Pc.Num() < 4) { continue; }
				// The plane through three corners holds the others.
				const FVector A(Pc[0], M.BlockZ(0, Pc[0].X, Pc[0].Y)), B(Pc[1], M.BlockZ(0, Pc[1].X, Pc[1].Y)), C(Pc[2], M.BlockZ(0, Pc[2].X, Pc[2].Y));
				const FVector N = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
				for (int32 k = 3; k < Pc.Num(); ++k)
				{
					const FVector V(Pc[k], M.BlockZ(0, Pc[k].X, Pc[k].Y));
					WorstOff = FMath::Max(WorstOff, FMath::Abs(FVector::DotProduct(V - A, N)));
				}
			}
			TestTrue(*FString::Printf(TEXT("cut %d: %d parts tile the polygon (%.2f of %.2f cm2)"), t, Parts.Num(), Sum, PolyArea(Poly)), FMath::Abs(Sum - PolyArea(Poly)) <= 1.0 + 0.5 * Parts.Num());
			TestTrue(*FString::Printf(TEXT("cut %d: every part is planar (worst %.6f cm off)"), t, WorstOff), WorstOff < 1e-6);
			TestTrue(*FString::Printf(TEXT("cut %d: more than one facet"), t), Parts.Num() > 1);
			// MinOver is the lowest point over the polygon: at or under every sample of it.
			const double Lo = M.MinOver(0, Poly), Hi = M.MaxOver(0, Poly);
			bool bBounds = true;
			for (int32 n = 0; n < 400; ++n)
			{
				const FVector2D P(FMath::Lerp(Tests[t].Min.X, Tests[t].Max.X, Rs.FRand()), FMath::Lerp(Tests[t].Min.Y, Tests[t].Max.Y, Rs.FRand()));
				const double Z = M.BlockZ(0, P.X, P.Y);
				bBounds &= Z >= Lo - 1e-6 && Z <= Hi + 1e-6;
			}
			TestTrue(*FString::Printf(TEXT("cut %d: MinOver / MaxOver bound the ground"), t), bBounds);
		}
	}

	// Panel offsets: text both ways, the offset under a point, the range under a box, a box cut into its panels' parts.
	{
		FRammsPanelOffsets P;
		P.Panels.Add({ FBox2D(FVector2D(0.0, 0.0), FVector2D(150.0, 100.0)), 0.8 });
		P.Panels.Add({ FBox2D(FVector2D(150.0, 0.0), FVector2D(300.0, 100.0)), -0.3 });
		FRammsPanelOffsets Q;
		Q.Parse(P.ToString());
		TestEqual(TEXT("panels round-trip"), Q.Panels.Num(), 2);
		TestTrue(TEXT("offset under a point"), FMath::Abs(Q.At(50.0, 50.0) - 0.8) < 1e-4 && FMath::Abs(Q.At(200.0, 50.0) + 0.3) < 1e-4 && Q.At(400.0, 50.0) == 0.0);
		double Lo = 0.0, Hi = 0.0;
		Q.Range(FBox2D(FVector2D(100.0, 20.0), FVector2D(200.0, 80.0)), Lo, Hi);
		TestTrue(TEXT("range over two panels"), FMath::Abs(Lo + 0.3) < 1e-4 && FMath::Abs(Hi - 0.8) < 1e-4);
		Q.Range(FBox2D(FVector2D(250.0, 20.0), FVector2D(350.0, 80.0)), Lo, Hi);
		TestTrue(TEXT("range reaching off the panels counts the model"), FMath::Abs(Lo + 0.3) < 1e-4 && FMath::Abs(Hi) < 1e-9);
		TArray<TPair<FBox2D, double>> Parts;
		const FBox2D Box(FVector2D(100.0, -50.0), FVector2D(350.0, 50.0));
		Q.Split(Box, Parts);
		double Sum = 0.0;
		for (const TPair<FBox2D, double>& Pt : Parts) { Sum += Pt.Key.GetArea(); }
		TestTrue(*FString::Printf(TEXT("split parts tile the box (%.1f of %.1f)"), Sum, Box.GetArea()), FMath::Abs(Sum - Box.GetArea()) < 1.0);
	}
	return true;
}

// --- a generated graded world --------------------------------------------------------------------------------------

namespace RammsGradedGroundTest
{
	/** A throwaway game world (with a physics scene, so traces work), torn down on destruction. */
	struct FScratchWorld
	{
		UWorld* World = nullptr;

		bool Open()
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			if (!World)
			{
				return false;
			}
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			return true;
		}

		~FScratchWorld()
		{
			if (World)
			{
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
			}
		}
	};

	struct FRun
	{
		FScratchWorld Scratch;
		ARammsWorldGenerator* Generator = nullptr;
		int32 Phase = 0;          // 0 first generation, 1 second generation, 2 done
		double PhaseStart = 0.0;
		bool bStarted = false;
		bool bSawGenerating = false;
		FString FirstSignature;
	};

	/** Everything the generation placed that the ground decides, as one sorted text: two equal ones are the same world. */
	FString Signature(UWorld* World, const ARammsWorldGenerator& Gen)
	{
		TArray<FString> Lines;
		for (TActorIterator<ARammsProcPiece> It(World); It; ++It)
		{
			const ARammsProcPiece* P = *It;
			const FVector L = P->GetActorLocation();
			const FRotator Rt = P->GetActorRotation();
			Lines.Add(FString::Printf(TEXT("P %d %.2f %.2f %.2f %.3f %.3f %.3f %.2f %.2f %.2f %.3f %s %s %.4f %08x"), static_cast<int32>(P->GetEffectiveKind()), L.X, L.Y, L.Z, Rt.Pitch, Rt.Yaw, Rt.Roll,
				P->SizeX, P->SizeY, P->SizeZ, P->Rise, *P->ManifestKind.ToString(), *P->ManifestSubtype.ToString(), P->Measure, FCrc::StrCrc32(*P->Parts)));
		}
		for (const FRammsManifestFeature& F : Gen.Manifest.Features)
		{
			Lines.Add(FString::Printf(TEXT("F %s %s %.2f %.2f %.2f %.3f %d"), *F.Kind.ToString(), *F.Subtype.ToString(), F.Location.X, F.Location.Y, F.Location.Z, F.Measure, F.bCompliant ? 1 : 0));
		}
		Lines.Sort();
		return FString::Join(Lines, TEXT("\n"));
	}

	TOptional<double> GroundUnder(UWorld* World, const FVector& At, double Above, double Below)
	{
		FHitResult Hit;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(RammsGradedProbe), /*bTraceComplex=*/true);
		if (World->LineTraceSingleByChannel(Hit, FVector(At.X, At.Y, At.Z + Above), FVector(At.X, At.Y, At.Z - Below), ECC_WorldStatic, Params))
		{
			return Hit.ImpactPoint.Z;
		}
		return TOptional<double>();
	}

	/** The checks on one generated world. */
	void CheckWorld(FAutomationTestBase& Test, UWorld* World, const ARammsWorldGenerator& Gen)
	{
		const FRammsWorldGenParams& Params = Gen.Params;
		const double VMax = Params.VerticalChangeMax;
		int32 Ramps = 0, Steps = 0, EntryRamps = 0, Humps = 0, Cracks = 0;

		// Curb ramps on graded ground follow it: just before the foot (on the ramp) and just past it (on the road) the
		// surface is within half a centimetre -- no slot at the curb line.
		for (const FRammsManifestFeature& F : Gen.Manifest.Features)
		{
			if (F.Kind != FName(TEXT("CurbRamp"))) { continue; }
			++Ramps;
			const FVector U = F.Rotation.RotateVector(FVector::ForwardVector).GetSafeNormal2D();
			const FVector Foot = F.Location + U * (F.Size.X * 0.5);
			const TOptional<double> OnRamp = GroundUnder(World, Foot - U * 1.5, 60.0, 60.0);
			const TOptional<double> OnRoad = GroundUnder(World, Foot + U * 1.5, 60.0, 60.0);
			if (Test.TestTrue(*FString::Printf(TEXT("curb ramp at %s: ground either side of its foot"), *F.Location.ToCompactString()), OnRamp.IsSet() && OnRoad.IsSet()))
			{
				Test.TestTrue(*FString::Printf(TEXT("curb ramp at %s: foot meets the road (%.2f vs %.2f)"), *F.Location.ToCompactString(), OnRamp.GetValue(), OnRoad.GetValue()), FMath::Abs(OnRamp.GetValue() - OnRoad.GetValue()) <= 0.5);
			}
		}

		for (TActorIterator<ARammsProcPiece> It(World); It; ++It)
		{
			const ARammsProcPiece* P = *It;
			const ERammsProcPieceKind K = P->GetEffectiveKind();
			const FTransform Xf = P->GetActorTransform();
			// Entrance steps stand on the ground: just in front of the bottom step the ground is at it, or above it by
			// less than a riser (the steps stand on the lowest ground under them).
			if (K == ERammsProcPieceKind::Step && P->ManifestKind == FName(TEXT("Step")))
			{
				++Steps;
				const FVector Front = Xf.TransformPosition(FVector(3.0, 0.0, 0.0));
				const TOptional<double> G = GroundUnder(World, FVector(Front.X, Front.Y, Xf.GetLocation().Z), 30.0, 60.0);
				if (Test.TestTrue(*FString::Printf(TEXT("steps at %s: ground in front"), *Xf.GetLocation().ToCompactString()), G.IsSet()))
				{
					const double Dz = G.GetValue() - Xf.GetLocation().Z;
					Test.TestTrue(*FString::Printf(TEXT("steps at %s: on the ground (ground %.2f cm above their foot)"), *Xf.GetLocation().ToCompactString(), Dz), Dz >= -0.6 && Dz <= FMath::Max(P->Rise, 1.0));
				}
			}
			// An entrance ramp's foot meets the ground beyond it.
			if (K == ERammsProcPieceKind::Ramp && P->ManifestKind == FName(TEXT("Ramp")))
			{
				++EntryRamps;
				const FVector Beyond = Xf.TransformPosition(FVector(3.0, 0.0, 0.0));
				const TOptional<double> G = GroundUnder(World, FVector(Beyond.X, Beyond.Y, Xf.GetLocation().Z), 30.0, 60.0);
				if (Test.TestTrue(*FString::Printf(TEXT("ramp at %s: ground beyond its foot"), *Xf.GetLocation().ToCompactString()), G.IsSet()))
				{
					const double Dz = G.GetValue() - Xf.GetLocation().Z;
					Test.TestTrue(*FString::Printf(TEXT("ramp at %s: its foot on the ground (%.2f cm)"), *Xf.GetLocation().ToCompactString(), Dz), FMath::Abs(Dz) <= 1.5);
				}
			}
			// A crack's record is on its piece.
			if (K == ERammsProcPieceKind::GroundPrisms && P->ManifestSubtype == FName(TEXT("Crack")))
			{
				++Cracks;
				FVector Origin, Extent;
				P->GetActorBounds(false, Origin, Extent);
				bool bFound = false;
				for (const FRammsManifestFeature& F : Gen.Manifest.Features)
				{
					if (F.Subtype == FName(TEXT("Crack")) && FVector::Dist2D(F.Location, Origin) <= 1.0) { bFound = true; break; }
				}
				Test.TestTrue(*FString::Printf(TEXT("crack at %s: its record is centred on it"), *Origin.ToCompactString()), bFound);
			}
		}

		// A speed hump's record is centred on its two halves, across the road.
		for (const FRammsManifestFeature& F : Gen.Manifest.Features)
		{
			if (F.Kind != FName(TEXT("SpeedHump"))) { continue; }
			++Humps;
			FBox Both(ForceInit);
			for (TActorIterator<ARammsProcPiece> It(World); It; ++It)
			{
				if (It->GetEffectiveKind() != ERammsProcPieceKind::Hump) { continue; }
				FVector Origin, Extent;
				It->GetActorBounds(false, Origin, Extent);
				if (InFeature(F, Origin, 10.0)) { Both += FBox(Origin - Extent, Origin + Extent); }
			}
			if (Test.TestTrue(*FString::Printf(TEXT("speed hump at %s: its halves are under its record"), *F.Location.ToCompactString()), Both.IsValid != 0))
			{
				Test.TestTrue(*FString::Printf(TEXT("speed hump at %s: the record is centred on them (%.1f cm off)"), *F.Location.ToCompactString(), FVector::Dist2D(Both.GetCenter(), F.Location)), FVector::Dist2D(Both.GetCenter(), F.Location) <= 10.0);
			}
		}

		// The walks' cross slopes are the settings', whatever the hill: never steeper than the steep blocks' draw.
		const double CrossCap = FMath::Max(Params.MaxCrossSlope * 3.0, 0.06) + 1e-3;
		int32 Sides = 0;
		for (const FRammsManifestFeature& F : Gen.Manifest.Features)
		{
			if (F.Kind != FName(TEXT("Grade")) || F.Subtype != FName(TEXT("Cross"))) { continue; }
			++Sides;
			Test.TestTrue(*FString::Printf(TEXT("side %s: cross slope %.4f within the settings (%.4f)"), *F.Route.ToString(), F.Measure, CrossCap), F.Measure <= CrossCap);
		}
		Test.TestTrue(TEXT("sides recorded"), Sides > 0);

		// Every lip over VerticalChangeMax along a route's centre line is recorded: walking each route at 2 cm steps, a
		// jump in the ground lies in some feature's box (a panel joint's record, a crack, a cover, a heave, a ramp, a prop).
		static const TSet<FName> Excuses = { FName(TEXT("Surface")), FName(TEXT("Cover")), FName(TEXT("Hazard")), FName(TEXT("CurbRamp")), FName(TEXT("Crossing")),
			FName(TEXT("Step")), FName(TEXT("Ramp")), FName(TEXT("Entrance")), FName(TEXT("Door")), FName(TEXT("Threshold")), FName(TEXT("Prop")), FName(TEXT("Tree")),
			FName(TEXT("Shrub")), FName(TEXT("Furniture")), FName(TEXT("Decor")), FName(TEXT("Vehicle")), FName(TEXT("SpeedHump")) };
		int32 Jumps = 0, Unrecorded = 0;
		FVector FirstUnrecorded = FVector::ZeroVector;
		for (const FRammsManifestRoute& Route : Gen.Manifest.Routes)
		{
			if (!Route.Name.ToString().StartsWith(TEXT("Block")) || Route.Points.Num() < 2) { continue; }
			TOptional<double> Prev;
			for (int32 k = 0; k + 1 < Route.Points.Num(); ++k)
			{
				const FVector A = Route.Points[k], B = Route.Points[k + 1];
				const int32 N = FMath::Max(1, FMath::CeilToInt(FVector::Dist2D(A, B) / 2.0));
				for (int32 s = 0; s < N; ++s)
				{
					const FVector P = FMath::Lerp(A, B, static_cast<double>(s) / N);
					const TOptional<double> G = GroundUnder(World, FVector(P.X, P.Y, Prev.IsSet() ? Prev.GetValue() : P.Z), 20.0, 30.0);
					if (G.IsSet() && Prev.IsSet() && FMath::Abs(G.GetValue() - Prev.GetValue()) > VMax + 0.05)
					{
						++Jumps;
						bool bRecorded = false;
						for (const FRammsManifestFeature& F : Gen.Manifest.Features)
						{
							if (Excuses.Contains(F.Kind) && InFeature(F, P, 25.0)) { bRecorded = true; break; }
						}
						if (!bRecorded)
						{
							if (Unrecorded == 0) { FirstUnrecorded = FVector(P.X, P.Y, G.GetValue()); }
							++Unrecorded;
						}
					}
					if (G.IsSet()) { Prev = G; }
				}
			}
		}
		Test.AddInfo(FString::Printf(TEXT("%d curb ramps, %d entrance steps, %d entrance ramps, %d humps, %d cracks; %d jumps over %.2f cm along the routes"), Ramps, Steps, EntryRamps, Humps, Cracks, Jumps, VMax));
		Test.TestTrue(TEXT("curb ramps were built"), Ramps > 0);
		Test.TestEqual(*FString::Printf(TEXT("every lip along the routes is recorded (first unrecorded at %s)"), *FirstUnrecorded.ToCompactString()), Unrecorded, 0);
	}
}

/** Waits for the generator, checks the world, regenerates once with the same seed and compares. */
class FRammsGradedGenerationCommand : public IAutomationLatentCommand
{
public:
	FRammsGradedGenerationCommand(FAutomationTestBase* InTest, TSharedRef<RammsGradedGroundTest::FRun> InRun) : Test(InTest), Run(InRun) {}

	virtual bool Update() override
	{
		using namespace RammsGradedGroundTest;
		UWorld* World = Run->Scratch.World;
		ARammsWorldGenerator* Gen = Run->Generator;
		if (!World || !Gen)
		{
			return true;
		}
		if (!Run->bStarted)
		{
			Run->bStarted = true;
			Run->PhaseStart = FPlatformTime::Seconds();
			if (!Test->TestTrue(TEXT("generation starts"), Gen->Regenerate()))
			{
				return true;
			}
		}
		// The scratch world is not in the engine's loop: tick it (the PCG subsystem runs the graph in its tick).
		World->Tick(LEVELTICK_All, 1.0f / 30.0f);
		const double Elapsed = FPlatformTime::Seconds() - Run->PhaseStart;
		const bool bGenerating = Gen->IsGenerating();
		Run->bSawGenerating |= bGenerating;
		if (bGenerating || !Run->bSawGenerating)
		{
			if (!Run->bSawGenerating && Elapsed > 20.0)
			{
				Test->AddError(TEXT("generation did not start within 20 s of ticking the world"));
				return true;
			}
			if (Elapsed > 300.0)
			{
				Test->AddError(TEXT("generation did not finish within 300 s"));
				return true;
			}
			return false;
		}
		if (!Test->TestTrue(TEXT("the generation wrote a manifest"), Gen->Manifest.Features.Num() > 0))
		{
			return true;
		}
		if (Run->Phase == 0)
		{
			CheckWorld(*Test, World, *Gen);
			Run->FirstSignature = Signature(World, *Gen);
			Run->Phase = 1;
			Run->bStarted = false;
			Run->bSawGenerating = false;
			return false;
		}
		const FString Second = Signature(World, *Gen);
		Test->TestTrue(*FString::Printf(TEXT("the same world twice from one seed (%d and %d characters)"), Run->FirstSignature.Len(), Second.Len()), Second == Run->FirstSignature);
		return true;
	}

private:
	FAutomationTestBase* Test;
	TSharedRef<RammsGradedGroundTest::FRun> Run;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsGradedGenerationTest, "Ramms.WorldGen.Graded.Generation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRammsGradedGenerationTest::RunTest(const FString& Parameters)
{
	using namespace RammsGradedGroundTest;
	TSharedRef<FRun> Run = MakeShared<FRun>();
	if (!TestTrue(TEXT("world opens"), Run->Scratch.Open()))
	{
		return false;
	}
	URammsWorldPreset* Preset = LoadObject<URammsWorldPreset>(nullptr, TEXT("/RammsWorldGen/Presets/PRE_GradedDowntown.PRE_GradedDowntown"));
	if (!TestNotNull(TEXT("PRE_GradedDowntown loads"), Preset))
	{
		return false;
	}
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ARammsWorldGenerator* Gen = Run->Scratch.World->SpawnActor<ARammsWorldGenerator>(ARammsWorldGenerator::StaticClass(), FTransform::Identity, SP);
	if (!TestNotNull(TEXT("generator spawns"), Gen))
	{
		return false;
	}
	Gen->Preset = Preset;
	Gen->Seed = 7;
	Gen->bSpawnArticulated = false;
	Gen->bPlacePlayersOnGenerate = false;
	// A small site (two blocks each way: corners, corner and mid-block crossings, a dropped street now and then), low
	// buildings set back behind yards so their entrances get steps and ramps, and every entrance stepped.
	const TPair<const TCHAR*, double> Overrides[] = {
		{ TEXT("SiteSizeX"), 11000.0 }, { TEXT("SiteSizeY"), 11000.0 }, { TEXT("FloorsMin"), 1.0 }, { TEXT("FloorsMax"), 3.0 },
		{ TEXT("SetbackMin"), 400.0 }, { TEXT("SetbackMax"), 500.0 }, { TEXT("EntranceStepChance"), 1.0 }, { TEXT("EntranceRampChance"), 1.0 },
		{ TEXT("SpeedHumpChance"), 1.0 }, { TEXT("CrackDensity"), 8.0 },
	};
	for (const TPair<const TCHAR*, double>& O : Overrides)
	{
		TestTrue(*FString::Printf(TEXT("parameter %s is set"), O.Key), Gen->SetParam(FName(O.Key), O.Value));
	}
	ADD_LATENT_AUTOMATION_COMMAND(FRammsGradedGenerationCommand(this, Run));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
