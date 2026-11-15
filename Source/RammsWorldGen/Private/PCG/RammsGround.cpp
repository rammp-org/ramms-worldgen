// Copyright ATDev. All Rights Reserved.
#include "PCG/RammsGround.h"

#include "Algo/Reverse.h"
#include "Math/RotationMatrix.h"
#include "RammsWorldPalette.h"
#include "RammsWorldGenModule.h"

namespace
{
	// Piecewise linear over the lines L with values F, linear beyond the first and last line. Optionally the slope of
	// the interval used (on a line: the interval to its +side, except past the last line).
	double Interp(const TArray<double>& L, const TArray<double>& F, double V, double* OutSlope = nullptr)
	{
		const int32 N = FMath::Min(L.Num(), F.Num());
		if (N <= 0)
		{
			if (OutSlope) { *OutSlope = 0.0; }
			return 0.0;
		}
		if (N == 1)
		{
			if (OutSlope) { *OutSlope = 0.0; }
			return F[0];
		}
		int32 k = 0;
		while (k < N - 2 && V >= L[k + 1]) { ++k; }
		const double Span = L[k + 1] - L[k];
		const double Slope = FMath::Abs(Span) > 1e-9 ? (F[k + 1] - F[k]) / Span : 0.0;
		if (OutSlope) { *OutSlope = Slope; }
		return F[k] + Slope * (V - L[k]);
	}

	// The line within Half of V (a hundredth of a millimetre of slack, so a point computed on a square's edge counts as
	// on it), or INDEX_NONE.
	int32 NearLine(const TArray<double>& L, double V, double Half)
	{
		for (int32 k = 0; k < L.Num(); ++k)
		{
			if (FMath::Abs(V - L[k]) <= Half + 1e-3) { return k; }
		}
		return INDEX_NONE;
	}

	FString JoinNumbers(const TArray<double>& V)
	{
		FString Out;
		for (int32 k = 0; k < V.Num(); ++k)
		{
			if (k > 0) { Out += TEXT(","); }
			Out += FString::Printf(TEXT("%.17g"), V[k]);
		}
		return Out;
	}

	void ParseNumbers(const FString& Text, TArray<double>& Out)
	{
		Out.Reset();
		TArray<FString> Items;
		Text.ParseIntoArray(Items, TEXT(","), true);
		for (const FString& S : Items)
		{
			Out.Add(FCString::Atod(*S));
		}
	}

	// Sorted, without values within Tol of the one before.
	void SortUnique(TArray<double>& V, double Tol)
	{
		V.Sort();
		TArray<double> Out;
		Out.Reserve(V.Num());
		for (const double X : V)
		{
			if (Out.Num() == 0 || X - Out.Last() > Tol) { Out.Add(X); }
		}
		V = MoveTemp(Out);
	}

	// A planar facet through a convex polygon's vertices with heights Zs (counter-clockwise; reversed when given the
	// other way). Its plane is the one through vertex 0 and the two vertices that span the widest triangle with it.
	bool MakeFace(const TArray<FVector2D>& InPoly, const TArray<double>& InZs, uint8 Zone, FRammsGroundModel::FFace& Out)
	{
		const int32 N = InPoly.Num();
		if (N < 3 || InZs.Num() != N)
		{
			return false;
		}
		const double Area = RammsGround::SignedArea(InPoly);
		if (FMath::Abs(Area) < 1e-3)
		{
			return false;
		}
		Out.Poly.Reset(N);
		TArray<double> Zs;
		Zs.Reserve(N);
		for (int32 k = 0; k < N; ++k)
		{
			const int32 s = Area > 0.0 ? k : N - 1 - k;
			Out.Poly.Add(InPoly[s]);
			Zs.Add(InZs[s]);
		}
		int32 Bi = 1, Bj = 2;
		double Best = -1.0;
		for (int32 i = 1; i < N; ++i)
		{
			for (int32 j = i + 1; j < N; ++j)
			{
				const FVector2D A = Out.Poly[i] - Out.Poly[0], B = Out.Poly[j] - Out.Poly[0];
				const double Cr = FMath::Abs(A.X * B.Y - A.Y * B.X);
				if (Cr > Best) { Best = Cr; Bi = i; Bj = j; }
			}
		}
		const FVector2D D1 = Out.Poly[Bi] - Out.Poly[0], D2 = Out.Poly[Bj] - Out.Poly[0];
		const double Det = D1.X * D2.Y - D1.Y * D2.X;
		if (FMath::Abs(Det) < 1e-9)
		{
			return false;
		}
		const double Z1 = Zs[Bi] - Zs[0], Z2 = Zs[Bj] - Zs[0];
		Out.G = FVector2D((Z1 * D2.Y - Z2 * D1.Y) / Det, (D1.X * Z2 - D2.X * Z1) / Det);
		Out.O = Out.Poly[0];
		Out.Z = Zs[0];
		Out.Zone = Zone;
		Out.Bounds = FBox2D(ForceInit);
		for (const FVector2D& P : Out.Poly) { Out.Bounds += P; }
		return true;
	}

	// Is P inside a convex counter-clockwise polygon (Tol cm of slack)?
	bool InConvex(const TArray<FVector2D>& Poly, const FVector2D& P, double Tol)
	{
		const int32 N = Poly.Num();
		for (int32 k = 0; k < N; ++k)
		{
			const FVector2D& A = Poly[k];
			const FVector2D E = Poly[(k + 1) % N] - A;
			const double Len = E.Size();
			if (Len < 1e-9) { continue; }
			const double Cr = E.X * (P.Y - A.Y) - E.Y * (P.X - A.X);
			if (Cr < -Tol * Len) { return false; }
		}
		return true;
	}

	double BoxDistance(const FBox2D& B, const FVector2D& P)
	{
		const double Dx = FMath::Max3(B.Min.X - P.X, 0.0, P.X - B.Max.X);
		const double Dy = FMath::Max3(B.Min.Y - P.Y, 0.0, P.Y - B.Max.Y);
		return FMath::Sqrt(Dx * Dx + Dy * Dy);
	}

	// One strip of the lot ring: the walk's back edge Outer (side-local t, height) at b = B0 and the ring's inner edge
	// Inner (t, height) at b = B1, both sorted by t, joined by triangles walking along both in step.
	void ZipStrip(const TArray<FVector2D>& Outer, const TArray<FVector2D>& Inner, double B0, double B1, TFunctionRef<FVector2D(double, double)> World,
		TArray<FRammsGroundModel::FFace>& Out)
	{
		int32 i = 0, j = 0;
		const int32 N = Outer.Num(), M = Inner.Num();
		if (N < 2 || M < 1)
		{
			return;
		}
		auto Tri = [&](const FVector2D& Pa, double Ba, const FVector2D& Pb, double Bb, const FVector2D& Pc, double Bc)
		{
			FRammsGroundModel::FFace F;
			if (MakeFace({ World(Pa.X, Ba), World(Pb.X, Bb), World(Pc.X, Bc) }, { Pa.Y, Pb.Y, Pc.Y }, 2, F)) { Out.Add(MoveTemp(F)); }
		};
		while (i < N - 1 || j < M - 1)
		{
			const bool bOuter = j >= M - 1 || (i < N - 1 && Outer[i + 1].X <= Inner[j + 1].X);
			if (bOuter)
			{
				Tri(Outer[i], B0, Outer[i + 1], B0, Inner[j], B1);
				++i;
			}
			else
			{
				Tri(Outer[i], B0, Inner[j + 1], B1, Inner[j], B1);
				++j;
			}
		}
	}
}

// --- FRammsGroundModel ------------------------------------------------------------------------------------------

FString FRammsGroundModel::ToString() const
{
	FString S = TEXT("G2|");
	S += FString::Printf(TEXT("%.17g,%.17g,%.17g,%.17g,%.17g,%.17g"), Z0, RoadWidth, SidewalkWidth, CurbHeight, Crown, CornerArm);
	S += TEXT("|") + JoinNumbers(X) + TEXT("|") + JoinNumbers(Y) + TEXT("|") + JoinNumbers(FX) + TEXT("|") + JoinNumbers(FY) + TEXT("|");
	for (int32 k = 0; k < Arms.Num(); ++k)
	{
		if (k > 0) { S += TEXT(","); }
		S += FString::Printf(TEXT("%d"), static_cast<int32>(Arms[k]));
	}
	S += TEXT("|");
	for (int32 b = 0; b < Blocks.Num(); ++b)
	{
		const FBlock& B = Blocks[b];
		if (b > 0) { S += TEXT(";"); }
		S += FString::Printf(TEXT("%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g"), B.X0, B.X1, B.Y0, B.Y1, B.Reveal, B.RevealX, B.RevealY, B.CrossSlope);
	}
	return S;
}

bool FRammsGroundModel::FromString(const FString& Text)
{
	TArray<FString> Sections;
	Text.ParseIntoArray(Sections, TEXT("|"), false);
	if (Sections.Num() < 8 || Sections[0] != TEXT("G2"))
	{
		return false;
	}
	TArray<double> Head;
	ParseNumbers(Sections[1], Head);
	if (Head.Num() < 6)
	{
		return false;
	}
	// Parsed into a copy, so a text that does not parse leaves this model as it was.
	FRammsGroundModel M;
	M.Z0 = Head[0]; M.RoadWidth = Head[1]; M.SidewalkWidth = Head[2]; M.CurbHeight = Head[3]; M.Crown = Head[4]; M.CornerArm = Head[5];
	ParseNumbers(Sections[2], M.X);
	ParseNumbers(Sections[3], M.Y);
	ParseNumbers(Sections[4], M.FX);
	ParseNumbers(Sections[5], M.FY);
	{
		TArray<FString> Items;
		Sections[6].ParseIntoArray(Items, TEXT(","), true);
		for (const FString& A : Items) { M.Arms.Add(static_cast<uint8>(FCString::Atoi(*A))); }
	}
	{
		TArray<FString> Items;
		Sections[7].ParseIntoArray(Items, TEXT(";"), true);
		for (const FString& It : Items)
		{
			TArray<double> V;
			ParseNumbers(It, V);
			if (V.Num() < 8) { return false; }
			FBlock& B = M.Blocks.AddDefaulted_GetRef();
			B.X0 = V[0]; B.X1 = V[1]; B.Y0 = V[2]; B.Y1 = V[3]; B.Reveal = V[4]; B.RevealX = V[5]; B.RevealY = V[6]; B.CrossSlope = V[7];
		}
	}
	if (!M.IsValid() || M.Arms.Num() != M.X.Num() * M.Y.Num())
	{
		return false;
	}
	*this = MoveTemp(M);
	Build();
	return true;
}

void FRammsGroundModel::Build()
{
	Faces.Reset();
	Faces.SetNum(Blocks.Num());
	if (!IsValid())
	{
		return;
	}
	for (int32 b = 0; b < Blocks.Num(); ++b)
	{
		BuildBlock(b, Faces[b]);
	}
}

void FRammsGroundModel::BuildBlock(int32 Bk, TArray<FFace>& Out) const
{
	Out.Reset();
	const FBox2D R = CurbRect(Bk);
	if (!R.bIsValid)
	{
		return;
	}
	const FVector2D Sz = R.GetSize();
	if (Sz.X < 8.0 || Sz.Y < 8.0)
	{
		return;
	}
	const double S = FMath::Clamp(SidewalkWidth, 1.0, FMath::Min(Sz.X, Sz.Y) * 0.5 - 2.0);
	const double C = Blocks[Bk].CrossSlope;
	const double KcBase = CornerArm > 0.0 ? CornerArm : S;

	// Corners counter-clockwise from the south-west; side k runs from corner k to corner k + 1 with the block on its
	// left (the Street Network's sides 0 south, 1 east, 2 north, 3 west: same starts, directions and inward normals).
	const FVector2D Cn[4] = { R.Min, FVector2D(R.Max.X, R.Min.Y), R.Max, FVector2D(R.Min.X, R.Max.Y) };
	const FVector2D Dir[4] = { FVector2D(1.0, 0.0), FVector2D(0.0, 1.0), FVector2D(-1.0, 0.0), FVector2D(0.0, -1.0) };
	double Len[4], Arm[4];
	for (int32 k = 0; k < 4; ++k)
	{
		Len[k] = (k % 2 == 0) ? Sz.X : Sz.Y;
		Arm[k] = S + FMath::Clamp(KcBase, 1.0, FMath::Max(1.0, (Len[k] - 2.0 * S) * 0.5 - 1.0));
	}
	auto In = [](const FVector2D& D) { return FVector2D(-D.Y, D.X); };
	auto Pt = [&](int32 k, double T, double B) { return Cn[k] + Dir[k] * T + In(Dir[k]) * B; };
	auto Edge = [&](int32 k, double T) { const FVector2D P = Pt(k, T, 0.0); return CurbTop(Bk, P.X, P.Y); };

	// Each side's walk back edge as (t, height), collected from the corners and the straight part, for the lot ring.
	TArray<FVector2D> Back[4];

	// --- straight walks: one plane per interval between the corner zones and the streets dropped inside the block ---
	TArray<double> Ix, Iy;
	InnerLines(Bk, Ix, Iy);
	for (int32 k = 0; k < 4; ++k)
	{
		TArray<double> Ts = { Arm[k], Len[k] - Arm[k] };
		const TArray<double>& Lines = (k % 2 == 0) ? Ix : Iy;
		for (const double V : Lines)
		{
			const double T = (k % 2 == 0) ? (V - Cn[k].X) / Dir[k].X : (V - Cn[k].Y) / Dir[k].Y;
			if (T > Arm[k] + 1e-3 && T < Len[k] - Arm[k] - 1e-3) { Ts.Add(T); }
		}
		SortUnique(Ts, 1e-6);
		for (int32 i = 0; i + 1 < Ts.Num(); ++i)
		{
			const double T0 = Ts[i], T1 = Ts[i + 1];
			const double E0 = Edge(k, T0), E1 = Edge(k, T1);
			FFace F;
			if (MakeFace({ Pt(k, T0, 0.0), Pt(k, T1, 0.0), Pt(k, T1, S), Pt(k, T0, S) }, { E0, E1, E1 + C * S, E0 + C * S }, 0, F)) { Out.Add(MoveTemp(F)); }
		}
		for (const double T : Ts) { Back[k].Add(FVector2D(T, Edge(k, T) + C * S)); }
	}

	// --- corner zones: a fan from the curb corner, each walk's own plane along its curb, the warp between ---
	constexpr int32 Sectors = 6;
	for (int32 j = 0; j < 4; ++j)
	{
		const int32 p = (j + 3) % 4; // the side arriving at corner j
		// Local (a, b): a along side j, b along side p backward (side j's inward); world = Cn[j] + u a + v b.
		const FVector2D U = Dir[j], V = In(Dir[j]);
		auto World = [&](double A, double B) { return Cn[j] + U * A + V * B; };
		const double E0 = CurbTop(Bk, Cn[j].X, Cn[j].Y);
		const double EA = Edge(j, Arm[j]);
		const double EB = Edge(p, Len[p] - Arm[p]);
		auto Zj = [&](double A, double B) { return E0 + (EA - E0) * A / Arm[j] + C * B; };
		auto Zp = [&](double A, double B) { return E0 + (EB - E0) * B / Arm[p] + C * A; };
		const double ThA = FMath::Atan2(S, Arm[j]);
		const double ThB = FMath::Atan2(Arm[p], S);
		TArray<double> Angles;
		for (int32 i = 0; i <= Sectors; ++i) { Angles.Add(ThA + (ThB - ThA) * i / Sectors); }
		Angles.Add(UE_DOUBLE_PI * 0.25);
		SortUnique(Angles, 1e-9);
		struct FQ { FVector2D Ab; double Z = 0.0; double Th = 0.0; };
		TArray<FQ> Qs;
		for (int32 i = 0; i < Angles.Num(); ++i)
		{
			const double Th = Angles[i];
			FVector2D Ab;
			if (i == 0) { Ab = FVector2D(Arm[j], S); }
			else if (i == Angles.Num() - 1) { Ab = FVector2D(S, Arm[p]); }
			else if (FMath::Abs(Th - UE_DOUBLE_PI * 0.25) < 1e-9) { Ab = FVector2D(S, S); }
			else if (Th < UE_DOUBLE_PI * 0.25) { Ab = FVector2D(S / FMath::Tan(Th), S); }
			else { Ab = FVector2D(S, S * FMath::Tan(Th)); }
			const double W = (ThB - Th) / (ThB - ThA);
			const double Z = (i == 0) ? EA + C * S : (i == Angles.Num() - 1) ? EB + C * S : W * Zj(Ab.X, Ab.Y) + (1.0 - W) * Zp(Ab.X, Ab.Y);
			Qs.Add({ Ab, Z, Th });
		}
		FFace F;
		if (MakeFace({ World(0.0, 0.0), World(Arm[j], 0.0), World(Arm[j], S) }, { E0, EA, EA + C * S }, 1, F)) { Out.Add(MoveTemp(F)); }
		for (int32 i = 0; i + 1 < Qs.Num(); ++i)
		{
			if (MakeFace({ World(0.0, 0.0), World(Qs[i].Ab.X, Qs[i].Ab.Y), World(Qs[i + 1].Ab.X, Qs[i + 1].Ab.Y) }, { E0, Qs[i].Z, Qs[i + 1].Z }, 1, F)) { Out.Add(MoveTemp(F)); }
		}
		if (MakeFace({ World(0.0, 0.0), World(S, Arm[p]), World(0.0, Arm[p]) }, { E0, EB + C * S, EB }, 1, F)) { Out.Add(MoveTemp(F)); }
		// The back-edge points: on side j's back edge (b = S) at t = a, on side p's (a = S) at t = Len[p] - b.
		for (const FQ& Q : Qs)
		{
			if (Q.Th <= UE_DOUBLE_PI * 0.25 + 1e-9) { Back[j].Add(FVector2D(Q.Ab.X, Q.Z)); }
			if (Q.Th >= UE_DOUBLE_PI * 0.25 - 1e-9) { Back[p].Add(FVector2D(Len[p] - Q.Ab.Y, Q.Z)); }
		}
	}

	// --- lots: the hill inside, a ring of triangles from it to the walks' back edges ---
	const FBox2D Lot(R.Min + FVector2D(S, S), R.Max - FVector2D(S, S));
	const FVector2D LotSz = Lot.GetSize();
	if (LotSz.X < 4.0 || LotSz.Y < 4.0)
	{
		return;
	}
	const double Ring = FMath::Clamp(FMath::Min(800.0, 0.35 * FMath::Min(LotSz.X, LotSz.Y)), 1.0, FMath::Min(LotSz.X, LotSz.Y) * 0.5 - 1.0);
	auto Hill = [&](const FVector2D& P) { return Base(P.X, P.Y) - CurbHeight + Reveal(Bk, P.X, P.Y) + C * S; };
	for (int32 k = 0; k < 4; ++k)
	{
		// The back edge, sorted and without repeats (each corner's point where the corner meets the straight part is
		// there twice, with the same height).
		TArray<FVector2D>& B = Back[k];
		B.Sort([](const FVector2D& A, const FVector2D& Bv) { return A.X < Bv.X; });
		TArray<FVector2D> Outer;
		for (const FVector2D& P : B)
		{
			if (Outer.Num() == 0 || P.X - Outer.Last().X > 1e-6) { Outer.Add(P); }
		}
		// The ring's inner edge: where the outer points face it, its ends, and where it crosses a dropped street.
		const double T0 = S + Ring, T1 = Len[k] - S - Ring;
		TArray<double> Ti = { T0, T1 };
		for (const FVector2D& P : Outer) { Ti.Add(FMath::Clamp(P.X, T0, T1)); }
		const TArray<double>& Lines = (k % 2 == 0) ? Ix : Iy;
		for (const double V : Lines)
		{
			const double T = (k % 2 == 0) ? (V - Cn[k].X) / Dir[k].X : (V - Cn[k].Y) / Dir[k].Y;
			if (T > T0 && T < T1) { Ti.Add(T); }
		}
		SortUnique(Ti, 1e-6);
		TArray<FVector2D> Inner;
		for (const double T : Ti) { Inner.Add(FVector2D(T, Hill(Pt(k, T, S + Ring)))); }
		ZipStrip(Outer, Inner, S, S + Ring, [&](double T, double Bv) { return Pt(k, T, Bv); }, Out);
	}
	// The inner rectangle on the hill, one plane per street-grid cell.
	const FBox2D Inner(Lot.Min + FVector2D(Ring, Ring), Lot.Max - FVector2D(Ring, Ring));
	TArray<TArray<FVector2D>> Cells = { RammsGround::BoxPoly(Inner) };
	for (const double V : Ix) { RammsGround::SplitAllByLine(Cells, FVector2D(1.0, 0.0), V); }
	for (const double V : Iy) { RammsGround::SplitAllByLine(Cells, FVector2D(0.0, 1.0), V); }
	for (const TArray<FVector2D>& Cell : Cells)
	{
		TArray<double> Zs;
		for (const FVector2D& P : Cell) { Zs.Add(Hill(P)); }
		FFace F;
		if (MakeFace(Cell, Zs, 3, F)) { Out.Add(MoveTemp(F)); }
	}
}

double FRammsGroundModel::Base(double InX, double InY) const
{
	return Z0 + Interp(X, FX, InX) + Interp(Y, FY, InY);
}

FVector2D FRammsGroundModel::BaseSlope(double InX, double InY) const
{
	double Sx = 0.0, Sy = 0.0;
	Interp(X, FX, InX, &Sx);
	Interp(Y, FY, InY, &Sy);
	return FVector2D(Sx, Sy);
}

FBox2D FRammsGroundModel::CurbRect(int32 Block) const
{
	if (!Blocks.IsValidIndex(Block))
	{
		return FBox2D(ForceInit);
	}
	const FBlock& B = Blocks[Block];
	const double H = RoadWidth * 0.5;
	return FBox2D(FVector2D(B.X0 + H, B.Y0 + H), FVector2D(B.X1 - H, B.Y1 - H));
}

int32 FRammsGroundModel::FindBlock(double InX, double InY, double Margin) const
{
	const double H = RoadWidth * 0.5 - Margin;
	for (int32 b = 0; b < Blocks.Num(); ++b)
	{
		const FBlock& B = Blocks[b];
		if (InX >= B.X0 + H && InX <= B.X1 - H && InY >= B.Y0 + H && InY <= B.Y1 - H)
		{
			return b;
		}
	}
	return INDEX_NONE;
}

void FRammsGroundModel::InnerLines(int32 Block, TArray<double>& OutX, TArray<double>& OutY) const
{
	OutX.Reset();
	OutY.Reset();
	const FBox2D R = CurbRect(Block);
	if (!R.bIsValid)
	{
		return;
	}
	for (const double V : X) { if (V > R.Min.X + 1.0 && V < R.Max.X - 1.0) { OutX.Add(V); } }
	for (const double V : Y) { if (V > R.Min.Y + 1.0 && V < R.Max.Y - 1.0) { OutY.Add(V); } }
}

double FRammsGroundModel::Reveal(int32 Block, double InX, double InY) const
{
	if (!Blocks.IsValidIndex(Block))
	{
		return CurbHeight;
	}
	const FBlock& B = Blocks[Block];
	return B.Reveal + B.RevealX * (InX - (B.X0 + B.X1) * 0.5) + B.RevealY * (InY - (B.Y0 + B.Y1) * 0.5);
}

double FRammsGroundModel::CurbTop(int32 Block, double InX, double InY) const
{
	return Base(InX, InY) - CurbHeight + Reveal(Block, InX, InY);
}

bool FRammsGroundModel::StraightRange(int32 Block, int32 Side, double& OutT0, double& OutT1) const
{
	const FBox2D R = CurbRect(Block);
	if (!R.bIsValid || Side < 0 || Side > 3)
	{
		return false;
	}
	const FVector2D Sz = R.GetSize();
	const double S = FMath::Clamp(SidewalkWidth, 1.0, FMath::Min(Sz.X, Sz.Y) * 0.5 - 2.0);
	const double L = (Side % 2 == 0) ? Sz.X : Sz.Y;
	const double Arm = S + FMath::Clamp(CornerArm > 0.0 ? CornerArm : S, 1.0, FMath::Max(1.0, (L - 2.0 * S) * 0.5 - 1.0));
	OutT0 = Arm;
	OutT1 = L - Arm;
	return true;
}

double FRammsGroundModel::RoadZ(double InX, double InY) const
{
	const double H = RoadWidth * 0.5;
	const double Gutter = Base(InX, InY) - CurbHeight;
	const int32 i = NearLine(X, InX, H);
	const int32 j = NearLine(Y, InY, H);
	double F = H; // distance from the crown line that sets the height: H = at the gutters
	if (i != INDEX_NONE && j != INDEX_NONE)
	{
		// An intersection square: the triangle toward an edge carries the crown of the road through that edge, or, where
		// that edge is a curb (no road arm), the crown of the other road. Continuous across the diagonals.
		const double Dx = InX - X[i], Dy = InY - Y[j];
		const double Ax = FMath::Abs(Dx), Ay = FMath::Abs(Dy);
		const int32 Node = j * X.Num() + i;
		const uint8 A = Arms.IsValidIndex(Node) ? Arms[Node] : static_cast<uint8>(ArmE | ArmN | ArmW | ArmS);
		if (Ax >= Ay)
		{
			const uint8 Bit = Dx >= 0.0 ? ArmE : ArmW;
			F = (A & Bit) ? Ay : Ax;
		}
		else
		{
			const uint8 Bit = Dy >= 0.0 ? ArmN : ArmS;
			F = (A & Bit) ? Ax : Ay;
		}
	}
	else if (j != INDEX_NONE)
	{
		F = FMath::Abs(InY - Y[j]);
	}
	else if (i != INDEX_NONE)
	{
		F = FMath::Abs(InX - X[i]);
	}
	return Gutter + Crown * (H - FMath::Min(F, H));
}

const FRammsGroundModel::FFace* FRammsGroundModel::FindFace(int32 Block, double InX, double InY) const
{
	if (!Faces.IsValidIndex(Block))
	{
		return nullptr;
	}
	const FVector2D P(InX, InY);
	const TArray<FFace>& Fs = Faces[Block];
	for (const FFace& F : Fs)
	{
		if (P.X < F.Bounds.Min.X - 1e-4 || P.X > F.Bounds.Max.X + 1e-4 || P.Y < F.Bounds.Min.Y - 1e-4 || P.Y > F.Bounds.Max.Y + 1e-4) { continue; }
		if (InConvex(F.Poly, P, 1e-4)) { return &F; }
	}
	// Just outside the curb rectangle (or in a hair's gap between facets): the nearest facet's plane.
	const FFace* Best = nullptr;
	double BestD = TNumericLimits<double>::Max();
	for (const FFace& F : Fs)
	{
		const double D = BoxDistance(F.Bounds, P);
		if (D < BestD) { BestD = D; Best = &F; }
	}
	return Best;
}

double FRammsGroundModel::BlockZ(int32 Block, double InX, double InY) const
{
	if (!Blocks.IsValidIndex(Block))
	{
		return RoadZ(InX, InY);
	}
	if (const FFace* F = FindFace(Block, InX, InY))
	{
		return F->At(InX, InY);
	}
	// No facets (not built): the curb's top.
	return CurbTop(Block, InX, InY);
}

double FRammsGroundModel::GroundZ(double InX, double InY) const
{
	const int32 B = FindBlock(InX, InY);
	return B != INDEX_NONE ? BlockZ(B, InX, InY) : RoadZ(InX, InY);
}

FVector FRammsGroundModel::Normal(double InX, double InY) const
{
	const double E = 1.0;
	const double Dzdx = (GroundZ(InX + E, InY) - GroundZ(InX - E, InY)) / (2.0 * E);
	const double Dzdy = (GroundZ(InX, InY + E) - GroundZ(InX, InY - E)) / (2.0 * E);
	return FVector(-Dzdx, -Dzdy, 1.0).GetSafeNormal();
}

void FRammsGroundModel::ClipToFaces(int32 Block, const TArray<FVector2D>& Poly, TArray<TArray<FVector2D>>& Out) const
{
	Out.Reset();
	if (!Faces.IsValidIndex(Block) || Poly.Num() < 3)
	{
		return;
	}
	TArray<FVector2D> P = Poly;
	if (RammsGround::SignedArea(P) < 0.0)
	{
		Algo::Reverse(P);
	}
	FBox2D Bb(ForceInit);
	for (const FVector2D& V : P) { Bb += V; }
	for (const FFace& F : Faces[Block])
	{
		if (F.Bounds.Max.X < Bb.Min.X || F.Bounds.Min.X > Bb.Max.X || F.Bounds.Max.Y < Bb.Min.Y || F.Bounds.Min.Y > Bb.Max.Y) { continue; }
		TArray<FVector2D> Piece;
		RammsGround::ClipConvex(P, F.Poly, Piece);
		if (Piece.Num() >= 3) { Out.Add(MoveTemp(Piece)); }
	}
}

double FRammsGroundModel::MinOver(int32 Block, const TArray<FVector2D>& Poly) const
{
	TArray<TArray<FVector2D>> Pieces;
	ClipToFaces(Block, Poly, Pieces);
	double Z = TNumericLimits<double>::Max();
	for (const TArray<FVector2D>& Pc : Pieces) { for (const FVector2D& V : Pc) { Z = FMath::Min(Z, BlockZ(Block, V.X, V.Y)); } }
	if (Pieces.Num() == 0)
	{
		for (const FVector2D& V : Poly) { Z = FMath::Min(Z, GroundZ(V.X, V.Y)); }
	}
	return Z;
}

double FRammsGroundModel::MaxOver(int32 Block, const TArray<FVector2D>& Poly) const
{
	TArray<TArray<FVector2D>> Pieces;
	ClipToFaces(Block, Poly, Pieces);
	double Z = TNumericLimits<double>::Lowest();
	for (const TArray<FVector2D>& Pc : Pieces) { for (const FVector2D& V : Pc) { Z = FMath::Max(Z, BlockZ(Block, V.X, V.Y)); } }
	if (Pieces.Num() == 0)
	{
		for (const FVector2D& V : Poly) { Z = FMath::Max(Z, GroundZ(V.X, V.Y)); }
	}
	return Z;
}

const FRammsGroundModel* FRammsGroundCache::Get(const FString& Text)
{
	if (Text.IsEmpty())
	{
		return nullptr;
	}
	if (Text != Key)
	{
		Key = Text;
		bValid = Model.FromString(Text);
		if (!bValid)
		{
			UE_LOG(LogRammsWorldGen, Warning, TEXT("Ground model text did not parse; the node places on the flat ground"));
		}
	}
	return bValid ? &Model : nullptr;
}

// --- panel offsets ----------------------------------------------------------------------------------------------

void FRammsPanelOffsets::Parse(const FString& Text)
{
	Panels.Reset();
	TArray<FString> Items;
	Text.ParseIntoArray(Items, TEXT(";"), true);
	for (const FString& It : Items)
	{
		TArray<double> V;
		ParseNumbers(It, V);
		if (V.Num() < 5) { continue; }
		FPanel& P = Panels.AddDefaulted_GetRef();
		P.R = FBox2D(FVector2D(FMath::Min(V[0], V[2]), FMath::Min(V[1], V[3])), FVector2D(FMath::Max(V[0], V[2]), FMath::Max(V[1], V[3])));
		P.Dz = V[4];
	}
}

FString FRammsPanelOffsets::ToString() const
{
	FString Out;
	for (const FPanel& P : Panels)
	{
		Out += FString::Printf(TEXT("%.2f,%.2f,%.2f,%.2f,%.4f;"), P.R.Min.X, P.R.Min.Y, P.R.Max.X, P.R.Max.Y, P.Dz);
	}
	return Out;
}

double FRammsPanelOffsets::At(double InX, double InY) const
{
	for (const FPanel& P : Panels)
	{
		if (InX >= P.R.Min.X && InX <= P.R.Max.X && InY >= P.R.Min.Y && InY <= P.R.Max.Y) { return P.Dz; }
	}
	return 0.0;
}

void FRammsPanelOffsets::Range(const FBox2D& Box, double& OutMin, double& OutMax) const
{
	OutMin = TNumericLimits<double>::Max();
	OutMax = TNumericLimits<double>::Lowest();
	double Covered = 0.0;
	for (const FPanel& P : Panels)
	{
		const double W = FMath::Min(Box.Max.X, P.R.Max.X) - FMath::Max(Box.Min.X, P.R.Min.X);
		const double H = FMath::Min(Box.Max.Y, P.R.Max.Y) - FMath::Max(Box.Min.Y, P.R.Min.Y);
		if (W <= 1e-3 || H <= 1e-3) { continue; }
		Covered += W * H;
		OutMin = FMath::Min(OutMin, P.Dz);
		OutMax = FMath::Max(OutMax, P.Dz);
	}
	// Listed panels never overlap: what they leave of the box is at the model's height.
	if (Covered < Box.GetArea() - 1.0)
	{
		OutMin = FMath::Min(OutMin, 0.0);
		OutMax = FMath::Max(OutMax, 0.0);
	}
}

void FRammsPanelOffsets::Split(const FBox2D& Box, TArray<TPair<FBox2D, double>>& Out) const
{
	Out.Reset();
	TArray<FBox2D> Rest = { Box };
	for (const FPanel& P : Panels)
	{
		const FBox2D I(FVector2D(FMath::Max(Box.Min.X, P.R.Min.X), FMath::Max(Box.Min.Y, P.R.Min.Y)), FVector2D(FMath::Min(Box.Max.X, P.R.Max.X), FMath::Min(Box.Max.Y, P.R.Max.Y)));
		if (I.Max.X - I.Min.X <= 0.5 || I.Max.Y - I.Min.Y <= 0.5) { continue; }
		Out.Add(TPair<FBox2D, double>(I, P.Dz));
		TArray<FBox2D> Next;
		for (const FBox2D& Rr : Rest) { RammsGround::SubtractBox(Rr, I, Next); }
		Rest = MoveTemp(Next);
	}
	for (const FBox2D& Rr : Rest)
	{
		if (Rr.Max.X - Rr.Min.X > 0.5 && Rr.Max.Y - Rr.Min.Y > 0.5) { Out.Add(TPair<FBox2D, double>(Rr, 0.0)); }
	}
}

// --- polygons ---------------------------------------------------------------------------------------------------

double RammsGround::SignedArea(const TArray<FVector2D>& Poly)
{
	double A = 0.0;
	for (int32 k = 0; k < Poly.Num(); ++k)
	{
		const FVector2D& P = Poly[k];
		const FVector2D& Q = Poly[(k + 1) % Poly.Num()];
		A += P.X * Q.Y - Q.X * P.Y;
	}
	return A * 0.5;
}

TArray<FVector2D> RammsGround::BoxPoly(const FBox2D& Box)
{
	return { Box.Min, FVector2D(Box.Max.X, Box.Min.Y), Box.Max, FVector2D(Box.Min.X, Box.Max.Y) };
}

void RammsGround::ClipHalfPlane(const TArray<FVector2D>& In, const FVector2D& N, double C, TArray<FVector2D>& Out)
{
	Out.Reset();
	const int32 Num = In.Num();
	if (Num < 3)
	{
		return;
	}
	for (int32 k = 0; k < Num; ++k)
	{
		const FVector2D& A = In[k];
		const FVector2D& B = In[(k + 1) % Num];
		const double Da = FVector2D::DotProduct(N, A) - C;
		const double Db = FVector2D::DotProduct(N, B) - C;
		if (Da <= 0.0)
		{
			Out.Add(A);
		}
		if ((Da < 0.0 && Db > 0.0) || (Da > 0.0 && Db < 0.0))
		{
			const double T = Da / (Da - Db);
			Out.Add(A + (B - A) * T);
		}
	}
	// Drop near-duplicate vertices a cut through a corner leaves.
	for (int32 k = Out.Num() - 1; k >= 0 && Out.Num() > 0; --k)
	{
		const FVector2D& Prev = Out[(k + Out.Num() - 1) % Out.Num()];
		if (Out.Num() > 1 && FVector2D::DistSquared(Out[k], Prev) < 1e-6)
		{
			Out.RemoveAt(k);
		}
	}
	if (Out.Num() < 3 || FMath::Abs(SignedArea(Out)) < 0.5)
	{
		Out.Reset();
	}
}

void RammsGround::SplitByLine(const TArray<FVector2D>& In, const FVector2D& N, double C, TArray<FVector2D>& Below, TArray<FVector2D>& Above)
{
	ClipHalfPlane(In, N, C, Below);
	ClipHalfPlane(In, -N, -C, Above);
}

void RammsGround::SplitAllByLine(TArray<TArray<FVector2D>>& Polys, const FVector2D& N, double C)
{
	TArray<TArray<FVector2D>> Out;
	Out.Reserve(Polys.Num());
	for (const TArray<FVector2D>& P : Polys)
	{
		double Lo = 1e300, Hi = -1e300;
		for (const FVector2D& V : P)
		{
			const double D = FVector2D::DotProduct(N, V) - C;
			Lo = FMath::Min(Lo, D);
			Hi = FMath::Max(Hi, D);
		}
		if (Lo >= -1e-6 || Hi <= 1e-6)
		{
			Out.Add(P);
			continue;
		}
		TArray<FVector2D> A, B;
		SplitByLine(P, N, C, A, B);
		if (A.Num() >= 3) { Out.Add(MoveTemp(A)); }
		if (B.Num() >= 3) { Out.Add(MoveTemp(B)); }
	}
	Polys = MoveTemp(Out);
}

void RammsGround::ClipConvex(const TArray<FVector2D>& A, const TArray<FVector2D>& B, TArray<FVector2D>& Out)
{
	Out = A;
	TArray<FVector2D> Tmp;
	const int32 N = B.Num();
	for (int32 k = 0; k < N && Out.Num() >= 3; ++k)
	{
		const FVector2D& P = B[k];
		const FVector2D E = B[(k + 1) % N] - P;
		// Inside a counter-clockwise polygon is left of each edge: E x (q - P) >= 0, i.e. (E.Y, -E.X) . q <= (E.Y, -E.X) . P.
		const FVector2D Nrm(E.Y, -E.X);
		ClipHalfPlane(Out, Nrm, FVector2D::DotProduct(Nrm, P), Tmp);
		Out = MoveTemp(Tmp);
		Tmp.Reset();
	}
	if (Out.Num() < 3)
	{
		Out.Reset();
	}
}

void RammsGround::SubtractBox(const FBox2D& R, const FBox2D& Cut, TArray<FBox2D>& Out)
{
	if (!(R.Min.X < Cut.Max.X && Cut.Min.X < R.Max.X && R.Min.Y < Cut.Max.Y && Cut.Min.Y < R.Max.Y))
	{
		Out.Add(R);
		return;
	}
	const double A0 = FMath::Max(Cut.Min.X, R.Min.X), A1 = FMath::Min(Cut.Max.X, R.Max.X);
	if (A0 - R.Min.X > 0.5) { Out.Add(FBox2D(R.Min, FVector2D(A0, R.Max.Y))); }
	if (R.Max.X - A1 > 0.5) { Out.Add(FBox2D(FVector2D(A1, R.Min.Y), R.Max)); }
	if (Cut.Min.Y - R.Min.Y > 0.5) { Out.Add(FBox2D(FVector2D(A0, R.Min.Y), FVector2D(A1, Cut.Min.Y))); }
	if (R.Max.Y - Cut.Max.Y > 0.5) { Out.Add(FBox2D(FVector2D(A0, Cut.Max.Y), FVector2D(A1, R.Max.Y))); }
}

double RammsGround::LerpTriangle(const FVector2D& A, const FVector2D& B, const FVector2D& C, double Va, double Vb, double Vc, const FVector2D& P)
{
	const FVector2D E1 = B - A, E2 = C - A, D = P - A;
	const double Det = E1.X * E2.Y - E1.Y * E2.X;
	if (FMath::Abs(Det) < 1e-12)
	{
		return Va;
	}
	const double U = (D.X * E2.Y - D.Y * E2.X) / Det;
	const double V = (E1.X * D.Y - E1.Y * D.X) / Det;
	return Va + U * (Vb - Va) + V * (Vc - Va);
}

// --- placing on a slope -------------------------------------------------------------------------------------------

FQuat RammsGround::OnPlane(double YawDeg, const FVector& N)
{
	FVector Up = N.GetSafeNormal();
	if (Up.IsNearlyZero())
	{
		Up = FVector::UpVector;
	}
	const double Yaw = FMath::DegreesToRadians(YawDeg);
	const FVector H(FMath::Cos(Yaw), FMath::Sin(Yaw), 0.0);
	FVector XAxis = H - FVector::DotProduct(H, Up) * Up;
	if (!XAxis.Normalize())
	{
		XAxis = FVector::ForwardVector;
	}
	return FRotationMatrix::MakeFromXZ(XAxis, Up).ToQuat();
}

FQuat RammsGround::FromAxes(const FVector& XDir, const FVector& N)
{
	FVector XAxis = XDir.GetSafeNormal();
	FVector Up = N.GetSafeNormal();
	if (XAxis.IsNearlyZero()) { XAxis = FVector::ForwardVector; }
	if (Up.IsNearlyZero()) { Up = FVector::UpVector; }
	return FRotationMatrix::MakeFromXZ(XAxis, Up).ToQuat();
}

int32 RammsGround::WriteCubeOn(FRammsPointWriter& W, const FVector& Centre, const FVector& Size, double YawDeg, const FVector& N, const FString& Material, int32 Seed)
{
	const FVector Scale(FMath::Max(Size.X, 0.1) / 100.0, FMath::Max(Size.Y, 0.1) / 100.0, FMath::Max(Size.Z, 0.1) / 100.0);
	const int32 i = W.Add(FTransform(OnPlane(YawDeg, N), Centre, Scale), FVector(-50.0), FVector(50.0), Seed);
	W.SetPath(i, RammsPCG::Mesh, FSoftObjectPath(RammsPCG::CubeMesh));
	W.SetPath(i, RammsPCG::Material, FSoftObjectPath(Material.IsEmpty() ? FString(RammsPCG::DefaultMaterial) : Material));
	return i;
}

int32 RammsGround::WritePickedMeshOn(FRammsPointWriter& W, const FRammsPaletteMesh& Picked, const FVector& Location, double YawDeg, const FVector& N, int32 Seed, double ScaleMul)
{
	const FQuat Facing = OnPlane(YawDeg, N);
	const FQuat Rot = Facing * Picked.RotationOffset.Quaternion();
	const FVector Loc = Location + Facing.RotateVector(Picked.LocationOffset * ScaleMul);
	const FVector Half(Picked.Footprint.X * 0.5 * ScaleMul, Picked.Footprint.Y * 0.5 * ScaleMul, Picked.Height * 0.5 * ScaleMul);
	const int32 i = W.Add(FTransform(Rot, Loc, FVector(Picked.Scale * ScaleMul)), FVector(-Half.X, -Half.Y, 0.0), FVector(Half.X, Half.Y, Picked.Height * ScaleMul), Seed);
	W.SetPath(i, RammsPCG::Mesh, Picked.Mesh.ToSoftObjectPath());
	W.Set(i, RammsPCG::ManifestCenter, Location);
	W.Set(i, RammsPCG::ManifestYaw, YawDeg);
	W.Set(i, RammsPCG::ManifestSize, FVector(Picked.Footprint.X * ScaleMul, Picked.Footprint.Y * ScaleMul, Picked.Height * ScaleMul));
	if (!Picked.SourceId.IsEmpty())
	{
		W.Set(i, RammsPCG::SourceId, Picked.SourceId);
	}
	return i;
}

// --- GroundPrisms -------------------------------------------------------------------------------------------------

bool RammsGround::ParseParts(const FString& Text, TArray<FPart>& Out)
{
	Out.Reset();
	TArray<FString> Items;
	Text.ParseIntoArray(Items, TEXT(";"), true);
	for (const FString& It : Items)
	{
		FString Head, Body;
		if (!It.Split(TEXT(":"), &Head, &Body))
		{
			continue;
		}
		FPart P;
		FString MatS, SurfS;
		if (Head.Split(TEXT(","), &MatS, &SurfS))
		{
			P.Mat = FMath::Clamp(FCString::Atoi(*MatS), 0, 2);
			P.Surface = FName(*SurfS);
		}
		else
		{
			P.Mat = FMath::Clamp(FCString::Atoi(*Head), 0, 2);
		}
		TArray<FString> Verts;
		Body.ParseIntoArray(Verts, TEXT(" "), true);
		for (const FString& V : Verts)
		{
			TArray<FString> C;
			V.ParseIntoArray(C, TEXT(","), true);
			if (C.Num() == 3)
			{
				P.Top.Add(FVector(FCString::Atod(*C[0]), FCString::Atod(*C[1]), FCString::Atod(*C[2])));
			}
		}
		if (P.Top.Num() >= 3)
		{
			Out.Add(MoveTemp(P));
		}
	}
	return Out.Num() > 0;
}

void RammsGround::FPrisms::Add(const TArray<FVector>& Top, int32 Mat, FName Surface)
{
	if (Top.Num() < 3)
	{
		return;
	}
	TArray<FVector2D> Plan;
	Plan.Reserve(Top.Num());
	for (const FVector& V : Top) { Plan.Add(FVector2D(V.X, V.Y)); }
	const double Area = SignedArea(Plan);
	if (FMath::Abs(Area) < 0.5)
	{
		return;
	}
	if (!Text.IsEmpty())
	{
		Text += TEXT(";");
	}
	Text += FString::Printf(TEXT("%d,%s:"), Mat, *Surface.ToString());
	const FQuat Rot = FRotator(0.0, Yaw, 0.0).Quaternion();
	const int32 Num3 = Top.Num();
	for (int32 k = 0; k < Num3; ++k)
	{
		// Counter-clockwise from above, whichever way it was given (a yaw keeps the winding).
		const FVector& V = Top[Area > 0.0 ? k : Num3 - 1 - k];
		const FVector L = Yaw == 0.0 ? V - Origin : Rot.UnrotateVector(V - Origin);
		if (k > 0) { Text += TEXT(" "); }
		Text += FString::Printf(TEXT("%.2f,%.2f,%.2f"), L.X, L.Y, L.Z);
		Extent += FVector2D(L.X, L.Y);
	}
	++Num;
}

void RammsGround::FPrisms::AddPoly(const TArray<FVector2D>& Poly, TFunctionRef<double(const FVector2D&)> Height, int32 Mat, FName Surface)
{
	if (Poly.Num() < 3)
	{
		return;
	}
	TArray<FVector> Top;
	Top.Reserve(Poly.Num());
	for (const FVector2D& P : Poly) { Top.Add(FVector(P.X, P.Y, Height(P))); }
	Add(Top, Mat, Surface);
}

void RammsGround::FPrisms::AddOnGround(const FRammsGroundModel& Ground, int32 Block, const TArray<FVector2D>& Poly, TFunctionRef<double(const FVector2D&)> Offset, int32 Mat, FName Surface)
{
	if (!Ground.Faces.IsValidIndex(Block) || Poly.Num() < 3)
	{
		return;
	}
	TArray<FVector2D> P = Poly;
	if (SignedArea(P) < 0.0)
	{
		Algo::Reverse(P);
	}
	FBox2D Bb(ForceInit);
	for (const FVector2D& V : P) { Bb += V; }
	// Each part takes its own facet's plane (not a search per corner), so it is planar to the last bit.
	for (const FRammsGroundModel::FFace& F : Ground.Faces[Block])
	{
		if (F.Bounds.Max.X < Bb.Min.X || F.Bounds.Min.X > Bb.Max.X || F.Bounds.Max.Y < Bb.Min.Y || F.Bounds.Min.Y > Bb.Max.Y) { continue; }
		TArray<FVector2D> Piece;
		ClipConvex(P, F.Poly, Piece);
		if (Piece.Num() >= 3)
		{
			AddPoly(Piece, [&](const FVector2D& Q) { return F.At(Q.X, Q.Y) + Offset(Q); }, Mat, Surface);
		}
	}
}

FRammsPieceDesc RammsGround::FPrisms::Desc(double Depth) const
{
	FRammsPieceDesc D;
	D.Kind = ERammsProcPieceKind::GroundPrisms;
	D.Transform = FTransform(FRotator(0.0, Yaw, 0.0), Origin);
	const FVector2D Sz = Extent.bIsValid ? Extent.GetSize() : FVector2D(100.0);
	D.SizeX = Sz.X;
	D.SizeY = Sz.Y;
	D.SizeZ = Depth;
	D.Parts = Text;
	return D;
}
