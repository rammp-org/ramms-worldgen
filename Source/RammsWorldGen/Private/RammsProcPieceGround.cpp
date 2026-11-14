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
	const FTransform Xf = NoScale(GetActorTransform());
	for (const RammsGround::FPart& P : Ps)
	{
		const double Bottom = MinTopZ(P.Top) - Depth;
		FRammsCollisionShape S;
		S.Surface = P.Surface.IsNone() ? FName(TEXT("Default")) : P.Surface;
		S.Source = GetFName();
		// A level rectangle along the actor's axes is a box (pavers, setts, panels on level ground); anything else a hull.
		bool bBox = P.Top.Num() == 4;
		double TopZ = P.Top[0].Z;
		FBox2D Plan(ForceInit);
		for (int32 k = 0; bBox && k < 4; ++k)
		{
			const FVector& A = P.Top[k];
			const FVector& B = P.Top[(k + 1) % 4];
			bBox = FMath::Abs(A.Z - TopZ) < 0.005 && (FMath::Abs(A.X - B.X) < 0.005 || FMath::Abs(A.Y - B.Y) < 0.005);
			Plan += FVector2D(A.X, A.Y);
		}
		if (bBox && Plan.bIsValid && Plan.GetSize().X > 0.01 && Plan.GetSize().Y > 0.01)
		{
			const FVector2D C = Plan.GetCenter(), E = Plan.GetExtent();
			S.Type = ERammsCollisionShapeType::Box;
			S.Transform = FTransform(Xf.GetRotation(), Xf.TransformPosition(FVector(C.X, C.Y, (TopZ + Bottom) * 0.5)));
			S.HalfExtents = FVector(E.X, E.Y, (TopZ - Bottom) * 0.5);
		}
		else
		{
			S.Type = ERammsCollisionShapeType::Convex;
			S.Transform = Xf;
			for (const FVector& V : P.Top)
			{
				S.Points.Add(FVector3f(V));
				S.Points.Add(FVector3f(static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(Bottom)));
			}
		}
		Out.Add(MoveTemp(S));
	}
}

void ARammsProcPiece::AppendHumpShapes(TArray<FRammsCollisionShape>& Out) const
{
	const double L = FMath::Max(SizeX, 20.0), W = FMath::Max(SizeY, 20.0), H = FMath::Max(Rise, 0.5), D = FMath::Max(SizeZ, 2.0);
	const int32 NX = HumpSamples(L);
	// The same grid as the mesh: samples along X at L * i / (NX - 1), both long edges (the profile is constant across).
	FRammsCollisionShape S;
	S.Type = ERammsCollisionShapeType::HeightGrid;
	S.Transform = FTransform(FVector(0.0, -W * 0.5, 0.0)) * NoScale(GetActorTransform());
	S.NumX = NX;
	S.NumY = 2;
	S.Spacing = FVector2D(L / (NX - 1), W);
	S.Heights.SetNum(NX * 2);
	for (int32 i = 0; i < NX; ++i)
	{
		const float Hz = static_cast<float>(HumpHeight(L * i / (NX - 1), L, H, LandingSize));
		S.Heights[i] = Hz;
		S.Heights[NX + i] = Hz;
	}
	S.BaseDepth = static_cast<float>(D);
	S.Surface = FName(TEXT("Asphalt"));
	S.Source = GetFName();
	Out.Add(MoveTemp(S));
}
