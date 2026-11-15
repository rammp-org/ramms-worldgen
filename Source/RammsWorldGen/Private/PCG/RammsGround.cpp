// Copyright ATDev. All Rights Reserved.
#include "PCG/RammsGround.h"

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

	bool ParseNumbers(const FString& Text, TArray<double>& Out)
	{
		Out.Reset();
		TArray<FString> Items;
		Text.ParseIntoArray(Items, TEXT(","), true);
		for (const FString& S : Items)
		{
			Out.Add(FCString::Atod(*S));
		}
		return true;
	}
}

// --- FRammsGroundModel ------------------------------------------------------------------------------------------

FString FRammsGroundModel::ToString() const
{
	FString S = TEXT("G1|");
	S += FString::Printf(TEXT("%.17g,%.17g,%.17g,%.17g,%.17g"), Z0, RoadWidth, SidewalkWidth, CurbHeight, Crown);
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
	if (Sections.Num() < 8 || Sections[0] != TEXT("G1"))
	{
		return false;
	}
	TArray<double> Head;
	ParseNumbers(Sections[1], Head);
	if (Head.Num() < 5)
	{
		return false;
	}
	Z0 = Head[0]; RoadWidth = Head[1]; SidewalkWidth = Head[2]; CurbHeight = Head[3]; Crown = Head[4];
	ParseNumbers(Sections[2], X);
	ParseNumbers(Sections[3], Y);
	ParseNumbers(Sections[4], FX);
	ParseNumbers(Sections[5], FY);
	Arms.Reset();
	{
		TArray<FString> Items;
		Sections[6].ParseIntoArray(Items, TEXT(","), true);
		for (const FString& A : Items) { Arms.Add(static_cast<uint8>(FCString::Atoi(*A))); }
	}
	Blocks.Reset();
	{
		TArray<FString> Items;
		Sections[7].ParseIntoArray(Items, TEXT(";"), true);
		for (const FString& It : Items)
		{
			TArray<double> V;
			ParseNumbers(It, V);
			if (V.Num() < 8) { continue; }
			FBlock& B = Blocks.AddDefaulted_GetRef();
			B.X0 = V[0]; B.X1 = V[1]; B.Y0 = V[2]; B.Y1 = V[3]; B.Reveal = V[4]; B.RevealX = V[5]; B.RevealY = V[6]; B.CrossSlope = V[7];
		}
	}
	return IsValid() && Arms.Num() == X.Num() * Y.Num();
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

double FRammsGroundModel::BlockZ(int32 Block, double InX, double InY) const
{
	if (!Blocks.IsValidIndex(Block))
	{
		return RoadZ(InX, InY);
	}
	const FBlock& B = Blocks[Block];
	const FBox2D R = CurbRect(Block);
	const double D = FMath::Clamp(FMath::Min(FMath::Min(InX - R.Min.X, R.Max.X - InX), FMath::Min(InY - R.Min.Y, R.Max.Y - InY)), 0.0, SidewalkWidth);
	return Base(InX, InY) - CurbHeight + Reveal(Block, InX, InY) + B.CrossSlope * D;
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
	const int32 Num3 = Top.Num();
	for (int32 k = 0; k < Num3; ++k)
	{
		// Counter-clockwise from above, whichever way it was given.
		const FVector& V = Top[Area > 0.0 ? k : Num3 - 1 - k];
		const FVector L = V - Origin;
		if (k > 0) { Text += TEXT(" "); }
		Text += FString::Printf(TEXT("%.2f,%.2f,%.2f"), L.X, L.Y, L.Z);
		Extent += FVector2D(V.X, V.Y);
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

FRammsPieceDesc RammsGround::FPrisms::Desc(double Depth) const
{
	FRammsPieceDesc D;
	D.Kind = ERammsProcPieceKind::GroundPrisms;
	D.Transform = FTransform(Origin);
	const FVector2D Sz = Extent.bIsValid ? Extent.GetSize() : FVector2D(100.0);
	D.SizeX = Sz.X;
	D.SizeY = Sz.Y;
	D.SizeZ = Depth;
	D.Parts = Text;
	return D;
}
