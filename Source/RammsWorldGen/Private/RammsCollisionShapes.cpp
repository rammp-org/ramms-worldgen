// Copyright ATDev. All Rights Reserved.
#include "RammsCollisionShapes.h"

#include "CompGeom/ConvexHull3.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "IndexTypes.h"

bool FRammsCollisionShapeFilter::Accepts(const FBox& WorldBounds) const
{
	if (IsEverything())
	{
		return true;
	}
	if (!WorldBounds.IsValid)
	{
		return false;
	}
	if (Region.IsValid && !Region.Intersect(WorldBounds))
	{
		return false;
	}
	if (Radius > 0.0)
	{
		const double Dx = FMath::Max3(WorldBounds.Min.X - Centre.X, 0.0, Centre.X - WorldBounds.Max.X);
		const double Dy = FMath::Max3(WorldBounds.Min.Y - Centre.Y, 0.0, Centre.Y - WorldBounds.Max.Y);
		if (Dx * Dx + Dy * Dy > Radius * Radius)
		{
			return false;
		}
	}
	return true;
}

namespace
{
	// Corners of a box of half extents H, bit k of the index picking +/- on axis k.
	FVector BoxCorner(const FVector& H, int32 Index)
	{
		return FVector((Index & 1) ? H.X : -H.X, (Index & 2) ? H.Y : -H.Y, (Index & 4) ? H.Z : -H.Z);
	}

	void AddQuad(TArray<FVector>& Out, const FVector& A, const FVector& B, const FVector& C, const FVector& D)
	{
		Out.Add(A); Out.Add(B); Out.Add(C);
		Out.Add(A); Out.Add(C); Out.Add(D);
	}

	// A sphere of rings x segments about Frame's Z, for drawing and probing spheres; a capsule is the same sphere split
	// at its equator, its halves pushed HalfLength up and down, with the straight part between them.
	void AddSphereTriangles(TArray<FVector>& Out, const FTransform& Frame, double Radius, double HalfLength)
	{
		constexpr int32 Rings = 8, Segments = 16;
		auto Point = [&](int32 Ring, int32 Segment, double Offset)
		{
			const double Phi = PI * Ring / Rings; // 0 at +Z
			const double Theta = 2.0 * PI * Segment / Segments;
			const FVector P(FMath::Sin(Phi) * FMath::Cos(Theta) * Radius, FMath::Sin(Phi) * FMath::Sin(Theta) * Radius, FMath::Cos(Phi) * Radius + Offset);
			return Frame.TransformPosition(P);
		};
		for (int32 r = 0; r < Rings; ++r)
		{
			const double Offset = r < Rings / 2 ? HalfLength : -HalfLength;
			for (int32 s = 0; s < Segments; ++s)
			{
				AddQuad(Out, Point(r, s, Offset), Point(r, s + 1, Offset), Point(r + 1, s + 1, Offset), Point(r + 1, s, Offset));
			}
		}
		if (HalfLength > 0.0)
		{
			for (int32 s = 0; s < Segments; ++s)
			{
				AddQuad(Out, Point(Rings / 2, s, HalfLength), Point(Rings / 2, s + 1, HalfLength), Point(Rings / 2, s + 1, -HalfLength), Point(Rings / 2, s, -HalfLength));
			}
		}
	}

	float LowestHeight(const FRammsCollisionShape& S)
	{
		float Low = S.Heights.Num() > 0 ? S.Heights[0] : 0.0f;
		for (const float H : S.Heights)
		{
			Low = FMath::Min(Low, H);
		}
		return Low;
	}
}

namespace RammsCollisionShapes
{
	FRammsCollisionShape& AddBox(TArray<FRammsCollisionShape>& Out, const FTransform& Frame, const FVector& HalfExtents, FName Surface, FName Source)
	{
		FRammsCollisionShape& S = Out.AddDefaulted_GetRef();
		S.Type = ERammsCollisionShapeType::Box;
		S.Transform = FTransform(Frame.GetRotation(), Frame.GetLocation());
		S.HalfExtents = HalfExtents.GetAbs();
		S.Surface = Surface;
		S.Source = Source;
		return S;
	}

	FRammsCollisionShape& AddConvex(TArray<FRammsCollisionShape>& Out, const FTransform& Frame, TArray<FVector3f>&& Points, FName Surface, FName Source)
	{
		FRammsCollisionShape& S = Out.AddDefaulted_GetRef();
		S.Type = ERammsCollisionShapeType::Convex;
		S.Transform = FTransform(Frame.GetRotation(), Frame.GetLocation());
		S.Points = MoveTemp(Points);
		S.Surface = Surface;
		S.Source = Source;
		return S;
	}

	FBox GetBounds(const FRammsCollisionShape& S)
	{
		FBox B(ForceInit);
		switch (S.Type)
		{
		case ERammsCollisionShapeType::Box:
			for (int32 k = 0; k < 8; ++k)
			{
				B += S.Transform.TransformPosition(BoxCorner(S.HalfExtents, k));
			}
			break;
		case ERammsCollisionShapeType::Convex:
			for (const FVector3f& P : S.Points)
			{
				B += S.Transform.TransformPosition(FVector(P));
			}
			break;
		case ERammsCollisionShapeType::HeightGrid:
		{
			for (int32 j = 0; j < S.NumY; ++j)
			{
				for (int32 i = 0; i < S.NumX; ++i)
				{
					const int32 K = j * S.NumX + i;
					B += S.Transform.TransformPosition(FVector(i * S.Spacing.X, j * S.Spacing.Y, S.Heights.IsValidIndex(K) ? S.Heights[K] : 0.0f));
				}
			}
			const double Base = LowestHeight(S) - S.BaseDepth;
			const double X1 = (S.NumX - 1) * S.Spacing.X, Y1 = (S.NumY - 1) * S.Spacing.Y;
			B += S.Transform.TransformPosition(FVector(0.0, 0.0, Base));
			B += S.Transform.TransformPosition(FVector(X1, 0.0, Base));
			B += S.Transform.TransformPosition(FVector(0.0, Y1, Base));
			B += S.Transform.TransformPosition(FVector(X1, Y1, Base));
			break;
		}
		case ERammsCollisionShapeType::Sphere:
			B = FBox::BuildAABB(S.Transform.GetLocation(), FVector(S.HalfExtents.X));
			break;
		case ERammsCollisionShapeType::Capsule:
		{
			const FVector Axis = S.Transform.GetUnitAxis(EAxis::Z) * S.HalfExtents.Z;
			B += S.Transform.GetLocation() + Axis;
			B += S.Transform.GetLocation() - Axis;
			B = B.ExpandBy(S.HalfExtents.X);
			break;
		}
		default:
			break;
		}
		return B;
	}

	void GetTriangles(const FRammsCollisionShape& S, TArray<FVector>& Out)
	{
		switch (S.Type)
		{
		case ERammsCollisionShapeType::Box:
		{
			FVector C[8];
			for (int32 k = 0; k < 8; ++k)
			{
				C[k] = S.Transform.TransformPosition(BoxCorner(S.HalfExtents, k));
			}
			AddQuad(Out, C[0], C[1], C[3], C[2]); // -Z
			AddQuad(Out, C[4], C[6], C[7], C[5]); // +Z
			AddQuad(Out, C[0], C[4], C[5], C[1]); // -Y
			AddQuad(Out, C[2], C[3], C[7], C[6]); // +Y
			AddQuad(Out, C[0], C[2], C[6], C[4]); // -X
			AddQuad(Out, C[1], C[5], C[7], C[3]); // +X
			break;
		}
		case ERammsCollisionShapeType::Convex:
		{
			TArray<FVector3d> P;
			P.Reserve(S.Points.Num());
			for (const FVector3f& V : S.Points)
			{
				P.Add(FVector3d(V));
			}
			UE::Geometry::FConvexHull3d Hull;
			if (P.Num() >= 4 && Hull.Solve(P.Num(), [&P](int32 Index) { return P[Index]; }))
			{
				for (const UE::Geometry::FIndex3i& T : Hull.GetTriangles())
				{
					Out.Add(S.Transform.TransformPosition(P[T.A]));
					Out.Add(S.Transform.TransformPosition(P[T.B]));
					Out.Add(S.Transform.TransformPosition(P[T.C]));
				}
			}
			break;
		}
		case ERammsCollisionShapeType::HeightGrid:
		{
			if (S.NumX < 2 || S.NumY < 2 || S.Heights.Num() < S.NumX * S.NumY)
			{
				break;
			}
			auto Top = [&](int32 i, int32 j) { return S.Transform.TransformPosition(FVector(i * S.Spacing.X, j * S.Spacing.Y, S.Heights[j * S.NumX + i])); };
			const double Base = LowestHeight(S) - S.BaseDepth;
			auto Bottom = [&](int32 i, int32 j) { return S.Transform.TransformPosition(FVector(i * S.Spacing.X, j * S.Spacing.Y, Base)); };
			for (int32 j = 0; j + 1 < S.NumY; ++j)
			{
				for (int32 i = 0; i + 1 < S.NumX; ++i)
				{
					// Split along (i + 1, j) - (i, j + 1), as the header says.
					Out.Add(Top(i, j)); Out.Add(Top(i + 1, j)); Out.Add(Top(i, j + 1));
					Out.Add(Top(i + 1, j)); Out.Add(Top(i + 1, j + 1)); Out.Add(Top(i, j + 1));
				}
			}
			// Walls straight down to the base, and the base.
			for (int32 i = 0; i + 1 < S.NumX; ++i)
			{
				AddQuad(Out, Top(i, 0), Top(i + 1, 0), Bottom(i + 1, 0), Bottom(i, 0));
				AddQuad(Out, Top(i, S.NumY - 1), Top(i + 1, S.NumY - 1), Bottom(i + 1, S.NumY - 1), Bottom(i, S.NumY - 1));
			}
			for (int32 j = 0; j + 1 < S.NumY; ++j)
			{
				AddQuad(Out, Top(0, j), Top(0, j + 1), Bottom(0, j + 1), Bottom(0, j));
				AddQuad(Out, Top(S.NumX - 1, j), Top(S.NumX - 1, j + 1), Bottom(S.NumX - 1, j + 1), Bottom(S.NumX - 1, j));
			}
			AddQuad(Out, Bottom(0, 0), Bottom(S.NumX - 1, 0), Bottom(S.NumX - 1, S.NumY - 1), Bottom(0, S.NumY - 1));
			break;
		}
		case ERammsCollisionShapeType::Sphere:
			AddSphereTriangles(Out, S.Transform, S.HalfExtents.X, 0.0);
			break;
		case ERammsCollisionShapeType::Capsule:
			AddSphereTriangles(Out, S.Transform, S.HalfExtents.X, S.HalfExtents.Z);
			break;
		default:
			break;
		}
	}

	TOptional<double> TopAt(const FRammsCollisionShape& Shape, double X, double Y)
	{
		if (Shape.Type == ERammsCollisionShapeType::Sphere || Shape.Type == ERammsCollisionShapeType::Capsule)
		{
			// Round shapes exactly: the highest point of the union of spheres of the radius along the axis segment.
			const double R = Shape.HalfExtents.X;
			const double Half = Shape.Type == ERammsCollisionShapeType::Capsule ? Shape.HalfExtents.Z : 0.0;
			const FVector C = Shape.Transform.GetLocation();
			const FVector Axis = Shape.Transform.GetUnitAxis(EAxis::Z);
			const int32 Samples = Half > 0.0 ? 4000 : 1;
			TOptional<double> Best;
			for (int32 k = 0; k < Samples; ++k)
			{
				const double S = Samples > 1 ? -Half + 2.0 * Half * k / (Samples - 1) : 0.0;
				const FVector Q = C + Axis * S;
				const double D2 = FMath::Square(X - Q.X) + FMath::Square(Y - Q.Y);
				if (D2 <= R * R)
				{
					const double Z = Q.Z + FMath::Sqrt(R * R - D2);
					if (!Best.IsSet() || Z > Best.GetValue())
					{
						Best = Z;
					}
				}
			}
			return Best;
		}
		TArray<FVector> Corners;
		GetTriangles(Shape, Corners);
		TOptional<double> Best;
		for (int32 t = 0; t + 2 < Corners.Num(); t += 3)
		{
			const FVector& A = Corners[t];
			const FVector& B = Corners[t + 1];
			const FVector& C = Corners[t + 2];
			const double D = (B.X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (B.Y - A.Y);
			if (FMath::Abs(D) < 1.0e-9)
			{
				continue; // seen edge-on from above
			}
			const double U = ((X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (Y - A.Y)) / D;
			const double V = ((B.X - A.X) * (Y - A.Y) - (X - A.X) * (B.Y - A.Y)) / D;
			constexpr double Eps = 1.0e-9;
			if (U < -Eps || V < -Eps || U + V > 1.0 + Eps)
			{
				continue;
			}
			const double Z = A.Z + U * (B.Z - A.Z) + V * (C.Z - A.Z);
			if (!Best.IsSet() || Z > Best.GetValue())
			{
				Best = Z;
			}
		}
		return Best;
	}

	int32 MatchHeightGridDiagonals(UE::Geometry::FDynamicMesh3& Mesh, int32 FirstVertex, const FTransform& GridFrame, int32 NumX, int32 NumY, const FVector2D& Spacing, TArray<float>& OutHeights)
	{
		using UE::Geometry::FDynamicMesh3;
		if (NumX < 2 || NumY < 2 || Spacing.X <= 0.0 || Spacing.Y <= 0.0)
		{
			return -1;
		}
		TArray<int32> Sample;
		Sample.Init(INDEX_NONE, NumX * NumY);
		OutHeights.Init(0.0f, NumX * NumY);
		for (const int32 Vid : Mesh.VertexIndicesItr())
		{
			if (Vid < FirstVertex)
			{
				continue;
			}
			const FVector L = GridFrame.InverseTransformPosition(Mesh.GetVertex(Vid));
			const double Fi = L.X / Spacing.X, Fj = L.Y / Spacing.Y;
			const int32 I = FMath::RoundToInt(Fi), J = FMath::RoundToInt(Fj);
			if (I < 0 || J < 0 || I >= NumX || J >= NumY || FMath::Abs(Fi - I) > 1.0e-3 || FMath::Abs(Fj - J) > 1.0e-3)
			{
				continue;
			}
			const int32 K = J * NumX + I;
			// A solid's bottom vertex shares the top's plan position: the top is the higher one.
			if (Sample[K] == INDEX_NONE || L.Z > OutHeights[K])
			{
				Sample[K] = Vid;
				OutHeights[K] = static_cast<float>(L.Z);
			}
		}
		for (const int32 Vid : Sample)
		{
			if (Vid == INDEX_NONE)
			{
				return -1;
			}
		}
		int32 Flips = 0;
		for (int32 j = 0; j + 1 < NumY; ++j)
		{
			for (int32 i = 0; i + 1 < NumX; ++i)
			{
				const int32 V00 = Sample[j * NumX + i], V10 = Sample[j * NumX + i + 1];
				const int32 V01 = Sample[(j + 1) * NumX + i], V11 = Sample[(j + 1) * NumX + i + 1];
				if (Mesh.FindEdge(V10, V01) != FDynamicMesh3::InvalidID)
				{
					continue;
				}
				const int32 Edge = Mesh.FindEdge(V00, V11);
				if (Edge == FDynamicMesh3::InvalidID)
				{
					continue; // not a quad of two triangles: nothing to choose
				}
				FDynamicMesh3::FEdgeFlipInfo Info;
				if (Mesh.FlipEdge(Edge, Info) == UE::Geometry::EMeshResult::Ok)
				{
					++Flips;
				}
			}
		}
		return Flips;
	}

	FName SurfaceForClass(FName SegmentationClass)
	{
		if (SegmentationClass.IsNone())
		{
			return NAME_None;
		}
		const FString C = SegmentationClass.ToString();
		if (C.StartsWith(TEXT("Hazard.")))
		{
			// The hazard's own surface where its subtype names one (Hazard.Mud, Hazard.Pond, Hazard.GravelTrail ...).
			struct FWord { const TCHAR* Word; const TCHAR* Surface; };
			static const FWord Words[] = {
				{ TEXT("Mud"), TEXT("Mud") }, { TEXT("Pond"), TEXT("Water") }, { TEXT("Water"), TEXT("Water") }, { TEXT("Puddle"), TEXT("Water") },
				{ TEXT("Gravel"), TEXT("Gravel") }, { TEXT("Cobble"), TEXT("Cobble") }, { TEXT("Grass"), TEXT("Grass") }, { TEXT("Mound"), TEXT("Grass") },
				{ TEXT("Lawn"), TEXT("Grass") }, { TEXT("Dirt"), TEXT("Dirt") }, { TEXT("Terrain"), TEXT("Dirt") }, { TEXT("Upheaval"), TEXT("Concrete") },
			};
			for (const FWord& W : Words)
			{
				if (C.Contains(W.Word))
				{
					return FName(W.Surface);
				}
			}
			return NAME_None;
		}
		struct FRule { const TCHAR* Class; const TCHAR* Surface; };
		static const FRule Exact[] = {
			{ TEXT("Road"), TEXT("Asphalt") }, { TEXT("Crosswalk"), TEXT("Asphalt") }, { TEXT("RoadMarking"), TEXT("Asphalt") },
			{ TEXT("Sidewalk"), TEXT("Concrete") }, { TEXT("Concrete"), TEXT("Concrete") }, { TEXT("Driveway"), TEXT("Concrete") },
			{ TEXT("CurbRamp"), TEXT("Concrete") }, { TEXT("DetectableWarning"), TEXT("Concrete") }, { TEXT("Stairs"), TEXT("Concrete") },
			{ TEXT("Ramp"), TEXT("Concrete") }, { TEXT("Path"), TEXT("Concrete") }, { TEXT("Slab"), TEXT("Concrete") },
			{ TEXT("Planter"), TEXT("Concrete") }, { TEXT("Column"), TEXT("Concrete") }, { TEXT("Curb"), TEXT("Curb") },
			{ TEXT("Cobblestone"), TEXT("Cobble") }, { TEXT("Lawn"), TEXT("Grass") }, { TEXT("Mound"), TEXT("Grass") },
			{ TEXT("Gravel"), TEXT("Gravel") }, { TEXT("Mud"), TEXT("Mud") }, { TEXT("Dirt"), TEXT("Dirt") }, { TEXT("Water"), TEXT("Water") },
			{ TEXT("Metal"), TEXT("Metal") }, { TEXT("Bollard"), TEXT("Metal") }, { TEXT("Threshold"), TEXT("Metal") },
			{ TEXT("Door"), TEXT("Wood") }, { TEXT("Trunk"), TEXT("Wood") }, { TEXT("Tree"), TEXT("Wood") }, { TEXT("Floor"), TEXT("Wood") },
			{ TEXT("Foliage"), TEXT("Foliage") }, { TEXT("Shrub"), TEXT("Foliage") }, { TEXT("Glass"), TEXT("Glass") },
			{ TEXT("Storefront"), TEXT("Glass") }, { TEXT("Wall.Exterior"), TEXT("Brick") }, { TEXT("Wall"), TEXT("Brick") },
		};
		for (const FRule& R : Exact)
		{
			if (C.Equals(R.Class))
			{
				return FName(R.Surface);
			}
		}
		// Facade parts ("Facade.<Part>") are building walls unless the part is glazing.
		if (C.StartsWith(TEXT("Facade.")))
		{
			return C.Contains(TEXT("Win")) || C.Contains(TEXT("Glass")) ? FName(TEXT("Glass")) : FName(TEXT("Brick"));
		}
		return NAME_None;
	}

	FName SurfaceForPhysicalMaterial(const FString& Name)
	{
		static const TCHAR* const Surfaces[] = {
			TEXT("Asphalt"), TEXT("Concrete"), TEXT("Curb"), TEXT("Brick"), TEXT("Cobble"), TEXT("Grass"), TEXT("Dirt"), TEXT("Gravel"),
			TEXT("Mud"), TEXT("Water"), TEXT("Metal"), TEXT("Wood"), TEXT("Glass"), TEXT("Foliage"),
		};
		for (const TCHAR* S : Surfaces)
		{
			if (Name.Contains(S, ESearchCase::IgnoreCase))
			{
				return FName(S);
			}
		}
		if (Name.Contains(TEXT("Stone"), ESearchCase::IgnoreCase))
		{
			return FName(TEXT("Cobble"));
		}
		if (Name.Contains(TEXT("Steel"), ESearchCase::IgnoreCase))
		{
			return FName(TEXT("Metal"));
		}
		return NAME_None;
	}

	const TCHAR* TypeName(ERammsCollisionShapeType Type)
	{
		switch (Type)
		{
		case ERammsCollisionShapeType::Box: return TEXT("Box");
		case ERammsCollisionShapeType::Convex: return TEXT("Convex");
		case ERammsCollisionShapeType::HeightGrid: return TEXT("HeightGrid");
		case ERammsCollisionShapeType::Sphere: return TEXT("Sphere");
		case ERammsCollisionShapeType::Capsule: return TEXT("Capsule");
		default: return TEXT("Unknown");
		}
	}
}
