// Copyright ATDev. All Rights Reserved.
// The ground kinds of ARammsProcPiece: graded streets and surface roughness (GroundPrisms, Hump), each with the exact
// collision shapes it describes (boxes, convex prisms, a height grid), so a lip stays a vertical step in every engine.
#include "RammsProcPiece.h"

#include "Components/DynamicMeshComponent.h"
#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "PCG/RammsGround.h"
#include "RammsCollisionShapes.h"
#include "RammsWorldGenModule.h"

namespace
{
	FGeometryScriptPrimitiveOptions GroundOpts(int32 MaterialID)
	{
		FGeometryScriptPrimitiveOptions O;
		O.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::PerFace;
		O.bFlipOrientation = false;
		O.UVMode = EGeometryScriptPrimitiveUVMode::Uniform;
		O.MaterialID = MaterialID;
		return O;
	}

	// One prism: the top polygon (counter-clockwise from above) and its footprint at BottomZ. Triangle winding follows
	// GeometryCore's convention (the face normal of (A, B, C) is (C - A) x (B - A), Unreal being left-handed).
	void AppendPrism(UE::Geometry::FDynamicMesh3& Mesh, UE::Geometry::FDynamicMeshMaterialAttribute* MatAttr, const TArray<FVector>& Top, double BottomZ, int32 Mat)
	{
		const int32 N = Top.Num();
		if (N < 3)
		{
			return;
		}
		TArray<int32> T, B;
		T.SetNum(N);
		B.SetNum(N);
		for (int32 k = 0; k < N; ++k)
		{
			T[k] = Mesh.AppendVertex(FVector3d(Top[k].X, Top[k].Y, Top[k].Z));
			B[k] = Mesh.AppendVertex(FVector3d(Top[k].X, Top[k].Y, BottomZ));
		}
		auto Tri = [&](int32 A, int32 Bv, int32 C)
		{
			const int32 Tid = Mesh.AppendTriangle(A, Bv, C);
			if (Tid >= 0 && MatAttr)
			{
				MatAttr->SetValue(Tid, Mat);
			}
		};
		for (int32 k = 1; k + 1 < N; ++k)
		{
			Tri(T[0], T[k + 1], T[k]);   // top, facing up
			Tri(B[0], B[k], B[k + 1]);   // bottom, facing down
		}
		for (int32 k = 0; k < N; ++k)
		{
			const int32 j = (k + 1) % N;
			Tri(T[k], B[j], B[k]);       // sides, facing out
			Tri(T[k], T[j], B[j]);
		}
	}

	double MinTopZ(const TArray<FVector>& Top)
	{
		double Z = TNumericLimits<double>::Max();
		for (const FVector& V : Top) { Z = FMath::Min(Z, V.Z); }
		return Z;
	}

	// A hump's or table's height above its base at X along it (0 at both ends).
	double HumpHeight(double X, double L, double H, double Plateau)
	{
		if (X <= 0.0 || X >= L)
		{
			return 0.0;
		}
		if (Plateau > 1.0)
		{
			const double Ramp = FMath::Max((L - FMath::Min(Plateau, L - 40.0)) * 0.5, 1.0);
			return H * FMath::Clamp(FMath::Min(X, L - X) / Ramp, 0.0, 1.0);
		}
		return H * FMath::Square(FMath::Sin(PI * X / L));
	}

	// The hump's grid along X (samples at L * i / (N - 1)), shared by the mesh and its height grid.
	int32 HumpSamples(double L)
	{
		return FMath::Clamp(FMath::RoundToInt(L / 2.0) + 1, 2, 401);
	}

	FTransform NoScale(const FTransform& Xf)
	{
		return FTransform(Xf.GetRotation(), Xf.GetLocation());
	}

	/** How far a prism's top may stray from one plane (cm) and still be exported as one hull (the text form rounds
	 *  vertices to 0.1 mm). */
	constexpr double PrismPlanarTolerance = 0.05;

	// Whether a prism's top polygon is planar (within Tolerance) and convex in plan: then the hull of it and its
	// footprint is the prism AppendPrism builds, whose top is a fan from the first vertex.
	bool IsPlanarConvex(const TArray<FVector>& Top, double Tolerance)
	{
		const int32 N = Top.Num();
		FVector Normal = FVector::ZeroVector, Mean = FVector::ZeroVector;
		for (int32 k = 0; k < N; ++k)
		{
			// Newell's normal: robust for any planar polygon.
			const FVector& A = Top[k];
			const FVector& B = Top[(k + 1) % N];
			Normal.X += (A.Y - B.Y) * (A.Z + B.Z);
			Normal.Y += (A.Z - B.Z) * (A.X + B.X);
			Normal.Z += (A.X - B.X) * (A.Y + B.Y);
			Mean += A;
		}
		Mean /= N;
		if (!Normal.Normalize())
		{
			return false;
		}
		for (const FVector& V : Top)
		{
			if (FMath::Abs(FVector::DotProduct(V - Mean, Normal)) > Tolerance)
			{
				return false;
			}
		}
		int32 Turn = 0;
		for (int32 k = 0; k < N; ++k)
		{
			const FVector& A = Top[k];
			const FVector& B = Top[(k + 1) % N];
			const FVector& C = Top[(k + 2) % N];
			const double Cross = (B.X - A.X) * (C.Y - B.Y) - (B.Y - A.Y) * (C.X - B.X);
			if (FMath::Abs(Cross) < 1.0e-6)
			{
				continue; // collinear
			}
			const int32 Sign = Cross > 0.0 ? 1 : -1;
			if (Turn != 0 && Sign != Turn)
			{
				return false;
			}
			Turn = Sign;
		}
		return true;
	}

	/** How far apart (cm) flush neighbours' tops may be and still be one solid: the text form's rounding, never a lip
	 *  (the smallest generated lip is millimetres). */
	constexpr double FlushTolerance = 0.01;

	/** A polygon's plane, Normal . p = Offset with Normal facing up (Newell's normal); false when it has no area. */
	bool PlaneOf(const TArray<FVector>& Top, FVector& OutNormal, double& OutOffset)
	{
		const int32 N = Top.Num();
		FVector Normal = FVector::ZeroVector, Mean = FVector::ZeroVector;
		for (int32 k = 0; k < N; ++k)
		{
			const FVector& A = Top[k];
			const FVector& B = Top[(k + 1) % N];
			Normal.X += (A.Y - B.Y) * (A.Z + B.Z);
			Normal.Y += (A.Z - B.Z) * (A.X + B.X);
			Normal.Z += (A.X - B.X) * (A.Y + B.Y);
			Mean += A;
		}
		if (N < 3 || !Normal.Normalize())
		{
			return false;
		}
		Mean /= N;
		if (Normal.Z < 0.0)
		{
			Normal = -Normal;
		}
		OutNormal = Normal;
		OutOffset = FVector::DotProduct(Normal, Mean);
		return true;
	}

	/** A polygon's area in plan. */
	double PlanArea(const TArray<FVector>& Top)
	{
		double A2 = 0.0;
		for (int32 k = 0; k < Top.Num(); ++k)
		{
			const FVector& P = Top[k];
			const FVector& Q = Top[(k + 1) % Top.Num()];
			A2 += P.X * Q.Y - Q.X * P.Y;
		}
		return FMath::Abs(A2) * 0.5;
	}

	/** The convex hull in plan of Points, their heights kept, counter-clockwise from above; a point within
	 *  FlushTolerance of the line through its neighbours is dropped (Andrew's monotone chain). */
	TArray<FVector> PlanHull(TArray<FVector> Points)
	{
		Points.Sort([](const FVector& A, const FVector& B) { return A.X < B.X || (A.X == B.X && A.Y < B.Y); });
		auto Turns = [](const FVector& O, const FVector& A, const FVector& B)
		{
			// Left of O->B by more than the tolerance (cross product over the base length).
			const double Cross = (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X);
			return Cross > FlushTolerance * FMath::Max(FVector::Dist2D(O, B), 1.0e-6);
		};
		const int32 N = Points.Num();
		TArray<FVector> H;
		H.SetNum(2 * N + 1);
		int32 K = 0;
		for (int32 i = 0; i < N; ++i)
		{
			while (K >= 2 && !Turns(H[K - 2], H[K - 1], Points[i]))
			{
				--K;
			}
			H[K++] = Points[i];
		}
		for (int32 i = N - 2, Lower = K + 1; i >= 0; --i)
		{
			while (K >= Lower && !Turns(H[K - 2], H[K - 1], Points[i]))
			{
				--K;
			}
			H[K++] = Points[i];
		}
		H.SetNum(FMath::Max(K - 1, 0));
		return H;
	}

	/** A GroundPrisms part on its way to being exported, or several flush parts merged into one solid. */
	struct FGroundSolid
	{
		/** The top polygon in plan (the union's hull once merged). */
		TArray<FVector> Top;
		/** Every merged prism's own corners: its top polygon and its own footprint at its own bottom. Their hull is the
		 *  solid, with the parts' bottoms where the mesh has them (a sloped part's bottom is not its neighbour's). */
		TArray<FVector> Corners;
		/** The lowest of the merged prisms' bottoms. */
		double Bottom = 0.0;
		double Area = 0.0;
		FName Surface;
		FVector Normal = FVector::UpVector;
		double Offset = 0.0;
		FBox2D Plan = FBox2D(ForceInit);
		bool bPlanarConvex = false;
		int32 Parts = 1;
		bool bAlive = true;
	};

	/** Whether two solids' tops are flush: the same surface, one plane (each top within FlushTolerance of the other's
	 *  plane), touching in plan. A lip of any height keeps them apart. */
	bool AreFlush(const FGroundSolid& A, const FGroundSolid& B)
	{
		if (!A.bPlanarConvex || !B.bPlanarConvex || A.Surface != B.Surface || !A.Plan.ExpandBy(FlushTolerance).Intersect(B.Plan))
		{
			return false;
		}
		auto Within = [](const FGroundSolid& S, const TArray<FVector>& Points)
		{
			for (const FVector& V : Points)
			{
				if (FMath::Abs(FVector::DotProduct(S.Normal, V) - S.Offset) > FlushTolerance)
				{
					return false;
				}
			}
			return true;
		};
		return Within(A, B.Top) && Within(B, A.Top);
	}

	/** Merge B into A when their union is convex (its hull's area is the sum of theirs): one solid with no seam. */
	bool MergeIfConvex(FGroundSolid& A, FGroundSolid& B)
	{
		TArray<FVector> Points = A.Top;
		Points.Append(B.Top);
		TArray<FVector> Hull = PlanHull(MoveTemp(Points));
		const double Sum = A.Area + B.Area;
		if (Hull.Num() < 3 || FMath::Abs(PlanArea(Hull) - Sum) > FMath::Max(1.0, Sum * 1.0e-6))
		{
			return false; // the union is not convex (an L, a gap between them): two solids
		}
		A.Top = MoveTemp(Hull);
		A.Corners.Append(B.Corners);
		A.Area = Sum;
		A.Bottom = FMath::Min(A.Bottom, B.Bottom);
		A.Plan += B.Plan;
		A.Parts += B.Parts;
		PlaneOf(A.Top, A.Normal, A.Offset);
		B.bAlive = false;
		return true;
	}
}

void ARammsProcPiece::BuildGroundPrisms(UDynamicMesh* M)
{
	TArray<RammsGround::FPart> Ps;
	if (!RammsGround::ParseParts(Parts, Ps))
	{
		return;
	}
	const double Depth = FMath::Max(SizeZ, 1.0);
	M->EditMesh([&](UE::Geometry::FDynamicMesh3& Mesh)
	{
		UE::Geometry::FDynamicMeshMaterialAttribute* MatAttr = Mesh.HasAttributes() ? Mesh.Attributes()->GetMaterialID() : nullptr;
		for (const RammsGround::FPart& P : Ps)
		{
			AppendPrism(Mesh, MatAttr, P.Top, MinTopZ(P.Top) - Depth, P.Mat);
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown);
}

void ARammsProcPiece::BuildHump(UDynamicMesh* M)
{
	const double L = FMath::Max(SizeX, 20.0), W = FMath::Max(SizeY, 20.0), H = FMath::Max(Rise, 0.5), D = FMath::Max(SizeZ, 2.0);
	const int32 NX = HumpSamples(L);
	int32 Start = 0;
	M->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh) { Start = Mesh.MaxVertexID(); });
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
		M, GroundOpts(0), FTransform(FVector(L * 0.5, 0.0, -D * 0.5)), static_cast<float>(L), static_cast<float>(W), static_cast<float>(D),
		NX, 2, 2, EGeometryScriptPrimitiveOriginMode::Center);
	M->EditMesh([&](UE::Geometry::FDynamicMesh3& Mesh)
	{
		for (const int32 Vid : Mesh.VertexIndicesItr())
		{
			if (Vid < Start) { continue; }
			FVector3d P = Mesh.GetVertex(Vid);
			if (P.Z > -D * 0.5)
			{
				P.Z = HumpHeight(P.X, L, H, LandingSize);
				Mesh.SetVertex(Vid, P);
			}
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown);
}

void ARammsProcPiece::AppendGroundPrismShapes(TArray<FRammsCollisionShape>& Out) const
{
	TArray<RammsGround::FPart> Ps;
	if (!RammsGround::ParseParts(Parts, Ps))
	{
		return;
	}
	const double Depth = FMath::Max(SizeZ, 1.0);
	// The mesh's frame; its scale (any sign) is baked into the shapes, as the capture does for the other kinds.
	const FTransform ToWorld = MeshComponent ? MeshComponent->GetComponentTransform() : GetActorTransform();
	const FVector Sc = ToWorld.GetScale3D();
	const FTransform Xf = NoScale(ToWorld);
	const FName Source = GetFName();

	// Flush neighbours (the same surface, tops in one plane, no lip between them) whose union is convex become one
	// solid: two geoms meeting at a seam give a body crossing it a contact on the far one's edge, a sideways push the
	// level surface does not have. A real lip keeps its two solids, and its step.
	TArray<FGroundSolid> Solids;
	Solids.Reserve(Ps.Num());
	for (const RammsGround::FPart& P : Ps)
	{
		FGroundSolid& S = Solids.AddDefaulted_GetRef();
		S.Top = P.Top;
		S.Bottom = MinTopZ(P.Top) - Depth;
		S.Corners.Reserve(P.Top.Num() * 2);
		for (const FVector& V : P.Top)
		{
			S.Corners.Add(V);
			S.Corners.Add(FVector(V.X, V.Y, S.Bottom));
		}
		S.Area = PlanArea(P.Top);
		S.Surface = P.Surface.IsNone() ? FName(TEXT("Default")) : P.Surface;
		S.bPlanarConvex = IsPlanarConvex(P.Top, PrismPlanarTolerance) && PlaneOf(P.Top, S.Normal, S.Offset);
		for (const FVector& V : P.Top)
		{
			S.Plan += FVector2D(V.X, V.Y);
		}
	}
	for (bool bMerged = true; bMerged;)
	{
		bMerged = false;
		for (int32 i = 0; i < Solids.Num(); ++i)
		{
			for (int32 j = i + 1; Solids[i].bAlive && j < Solids.Num(); ++j)
			{
				if (Solids[j].bAlive && AreFlush(Solids[i], Solids[j]) && MergeIfConvex(Solids[i], Solids[j]))
				{
					bMerged = true;
				}
			}
		}
	}

	for (const FGroundSolid& S : Solids)
	{
		if (!S.bAlive)
		{
			continue;
		}
		const TArray<FVector>& Top = S.Top;
		const double Bottom = S.Bottom;
		// A level rectangle along the actor's axes is a box (pavers, setts, panels on level ground); a planar convex top
		// the hull of it and its footprint; anything else (twisted, not convex) the mesh's own fan, prism by prism.
		bool bBox = Top.Num() == 4;
		const double TopZ = Top[0].Z;
		FBox2D Plan(ForceInit);
		for (int32 k = 0; bBox && k < 4; ++k)
		{
			const FVector& A = Top[k];
			const FVector& B = Top[(k + 1) % 4];
			bBox = FMath::Abs(A.Z - TopZ) < 0.005 && (FMath::Abs(A.X - B.X) < 0.005 || FMath::Abs(A.Y - B.Y) < 0.005);
			Plan += FVector2D(A.X, A.Y);
		}
		if (bBox && Plan.bIsValid && Plan.GetSize().X > 0.01 && Plan.GetSize().Y > 0.01)
		{
			// Scaled along the actor's axes (mirrored too), a box along them stays one.
			const FVector2D C = Plan.GetCenter(), E = Plan.GetExtent();
			const FVector Centre(C.X, C.Y, (TopZ + Bottom) * 0.5);
			RammsCollisionShapes::AddBox(Out, FTransform(Xf.GetRotation(), Xf.TransformPosition(Centre * Sc)), FVector(E.X, E.Y, (TopZ - Bottom) * 0.5) * Sc.GetAbs(), S.Surface, Source);
			continue;
		}
		if (S.bPlanarConvex)
		{
			// The hull of the prisms' own corners: the top exact, the bottom where each part's is (a single part: its
			// top and its footprint).
			TArray<FVector3f> Points;
			Points.Reserve(S.Corners.Num());
			for (const FVector& V : S.Corners)
			{
				Points.Add(FVector3f(V * Sc));
			}
			RammsCollisionShapes::AddConvex(Out, Xf, MoveTemp(Points), S.Surface, Source);
			continue;
		}
		static TSet<FName> Reported;
		if (!Reported.Contains(Source))
		{
			Reported.Add(Source);
			UE_LOG(LogRammsWorldGen, Log, TEXT("%s: a ground prism's top is not planar and convex; exported as the mesh's own triangles, one prism each"), *Source.ToString());
		}
		for (int32 k = 1; k + 1 < Top.Num(); ++k)
		{
			// AppendPrism's top fan, triangle for triangle (a solid that is not planar and convex is never merged).
			RammsCollisionShapes::AddTrianglePrism(Out, Xf, Sc, Top[0], Top[k + 1], Top[k], Bottom, S.Surface, Source);
		}
	}
}

void ARammsProcPiece::AppendHumpShapes(TArray<FRammsCollisionShape>& Out) const
{
	const double L = FMath::Max(SizeX, 20.0), W = FMath::Max(SizeY, 20.0), H = FMath::Max(Rise, 0.5), D = FMath::Max(SizeZ, 2.0);
	const int32 NX = HumpSamples(L);
	const FTransform ToWorld = MeshComponent ? MeshComponent->GetComponentTransform() : GetActorTransform();
	const FVector Sc = ToWorld.GetScale3D();
	const FTransform Xf = NoScale(ToWorld);
	const FName Surface(TEXT("Asphalt"));
	auto HeightAt = [&](int32 i) { return HumpHeight(L * i / (NX - 1), L, H, LandingSize); };
	if (Sc.X <= 0.0 || Sc.Y <= 0.0 || Sc.Z <= 0.0)
	{
		// Mirrored or turned over it is no height grid; the profile is constant across, so each cell along X is a
		// convex column (its top planar) and the hulls are exact.
		for (int32 i = 0; i + 1 < NX; ++i)
		{
			const double X0 = L * i / (NX - 1), X1 = L * (i + 1) / (NX - 1), H0 = HeightAt(i), H1 = HeightAt(i + 1);
			TArray<FVector3f> Points;
			for (const double Y : { -W * 0.5, W * 0.5 })
			{
				Points.Add(FVector3f(FVector(X0, Y, H0) * Sc));
				Points.Add(FVector3f(FVector(X1, Y, H1) * Sc));
				Points.Add(FVector3f(FVector(X0, Y, -D) * Sc));
				Points.Add(FVector3f(FVector(X1, Y, -D) * Sc));
			}
			RammsCollisionShapes::AddConvex(Out, Xf, MoveTemp(Points), Surface, GetFName());
		}
		return;
	}
	// The same grid as the mesh: samples along X at L * i / (NX - 1), both long edges (the profile is constant across).
	FRammsCollisionShape S;
	S.Type = ERammsCollisionShapeType::HeightGrid;
	S.Transform = FTransform(FVector(0.0, -W * 0.5, 0.0) * Sc) * Xf;
	S.NumX = NX;
	S.NumY = 2;
	S.Spacing = FVector2D(L / (NX - 1) * Sc.X, W * Sc.Y);
	S.Heights.SetNum(NX * 2);
	for (int32 i = 0; i < NX; ++i)
	{
		const float Hz = static_cast<float>(HeightAt(i) * Sc.Z);
		S.Heights[i] = Hz;
		S.Heights[NX + i] = Hz;
	}
	S.BaseDepth = static_cast<float>(D * Sc.Z);
	S.Surface = Surface;
	S.Source = GetFName();
	Out.Add(MoveTemp(S));
}
