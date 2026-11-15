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

	/** Only the points on the hull of Points (in their original order), or all of them when they span no volume. */
	TArray<FVector> HullPoints(TArray<FVector>&& Points)
	{
		UE::Geometry::FConvexHull3d Hull;
		if (Points.Num() < 4 || !Hull.Solve(Points.Num(), [&Points](int32 Index) { return Points[Index]; }))
		{
			return MoveTemp(Points);
		}
		TArray<bool> Used;
		Used.Init(false, Points.Num());
		for (const UE::Geometry::FIndex3i& T : Hull.GetTriangles())
		{
			Used[T.A] = Used[T.B] = Used[T.C] = true;
		}
		TArray<FVector> Out;
		for (int32 i = 0; i < Points.Num(); ++i)
		{
			if (Used[i])
			{
				Out.Add(Points[i]);
			}
		}
		return Out;
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

	void AddTrianglePrism(TArray<FRammsCollisionShape>& Out, const FTransform& Frame, const FVector& Scale, const FVector& A, const FVector& B, const FVector& C, double BottomZ, FName Surface, FName Source)
	{
		const double Area2 = (B.X - A.X) * (C.Y - A.Y) - (C.X - A.X) * (B.Y - A.Y);
		const double Top = FMath::Max3(A.Z, B.Z, C.Z);
		if (FMath::Abs(Area2) < 1.0e-6 || Top - BottomZ < 1.0e-3)
		{
			return; // edge-on from above, or no height above the bottom: no solid
		}
		const FVector Corners[3] = { A, B, C };
		TArray<FVector3f> Points;
		Points.Reserve(6);
		for (const FVector& P : Corners)
		{
			Points.Add(FVector3f(P * Scale));
			Points.Add(FVector3f(FVector(P.X, P.Y, BottomZ) * Scale));
		}
		AddConvex(Out, Frame, MoveTemp(Points), Surface, Source);
	}

	int32 MergeFlushBoxes(TArray<FRammsCollisionShape>& Shapes, int32 FirstIndex, double Tolerance)
	{
		// A box in the frame of its own orientation: there, two boxes of one orientation merge along an axis when their
		// other two axes agree, so each axis is a sort over buckets of boxes that agree on the other two.
		struct FBoxRef
		{
			int32 Index;
			FQuat Rotation;
			FVector Local;
		};
		const double Tol = FMath::Max(Tolerance, 1.0e-6);
		auto Quantized = [Tol](double V) { return GetTypeHash(FMath::RoundToInt64(V / Tol)); };
		TArray<bool> Dead;
		Dead.Init(false, Shapes.Num());
		int32 Merged = 0;
		for (int32 Pass = 0; Pass < 2; ++Pass) // a second pass joins rows the first one made
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				TMap<uint32, TArray<FBoxRef>> Buckets;
				for (int32 i = FMath::Max(FirstIndex, 0); i < Shapes.Num(); ++i)
				{
					const FRammsCollisionShape& S = Shapes[i];
					if (Dead[i] || S.Type != ERammsCollisionShapeType::Box)
					{
						continue;
					}
					FQuat R = S.Transform.GetRotation().GetNormalized();
					if (R.W < 0.0)
					{
						R = FQuat(-R.X, -R.Y, -R.Z, -R.W);
					}
					const FVector L = R.UnrotateVector(S.Transform.GetLocation());
					uint32 Hash = HashCombine(GetTypeHash(S.Surface), GetTypeHash(Axis));
					const double Components[4] = { R.X, R.Y, R.Z, R.W };
					for (const double C : Components)
					{
						Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt64(C * 1.0e6)));
					}
					for (int32 k = 0; k < 3; ++k)
					{
						if (k != Axis)
						{
							Hash = HashCombine(Hash, Quantized(L[k]));
							Hash = HashCombine(Hash, Quantized(S.HalfExtents[k]));
						}
					}
					Buckets.FindOrAdd(Hash).Add({ i, R, L });
				}
				for (TPair<uint32, TArray<FBoxRef>>& Bucket : Buckets)
				{
					TArray<FBoxRef>& B = Bucket.Value;
					if (B.Num() < 2)
					{
						continue;
					}
					B.Sort([Axis](const FBoxRef& X, const FBoxRef& Y) { return X.Local[Axis] < Y.Local[Axis]; });
					int32 Cur = 0;
					for (int32 n = 1; n < B.Num(); ++n)
					{
						FRammsCollisionShape& A = Shapes[B[Cur].Index];
						const FRammsCollisionShape& Next = Shapes[B[n].Index];
						// The bucket is a hash: check what it stands for.
						bool bAligned = A.Surface == Next.Surface && FMath::Abs(B[Cur].Rotation | B[n].Rotation) > 1.0 - 1.0e-9;
						for (int32 k = 0; bAligned && k < 3; ++k)
						{
							bAligned = k == Axis || (FMath::Abs(B[Cur].Local[k] - B[n].Local[k]) <= Tol && FMath::Abs(A.HalfExtents[k] - Next.HalfExtents[k]) <= Tol);
						}
						const double Lo = B[Cur].Local[Axis] - A.HalfExtents[Axis];
						const double End = B[Cur].Local[Axis] + A.HalfExtents[Axis];
						const double Start = B[n].Local[Axis] - Next.HalfExtents[Axis];
						if (!bAligned || FMath::Abs(End - Start) > Tol)
						{
							Cur = n; // a gap, an overlap or a lip: they stay two
							continue;
						}
						const double Hi = B[n].Local[Axis] + Next.HalfExtents[Axis];
						B[Cur].Local[Axis] = (Lo + Hi) * 0.5;
						A.HalfExtents[Axis] = (Hi - Lo) * 0.5;
						A.Transform.SetLocation(B[Cur].Rotation.RotateVector(B[Cur].Local));
						Dead[B[n].Index] = true;
						++Merged;
					}
				}
			}
		}
		if (Merged > 0)
		{
			int32 Write = FMath::Max(FirstIndex, 0);
			for (int32 i = Write; i < Shapes.Num(); ++i)
			{
				if (!Dead[i])
				{
					if (Write != i)
					{
						Shapes[Write] = MoveTemp(Shapes[i]);
					}
					++Write;
				}
			}
			Shapes.SetNum(Write);
		}
		return Merged;
	}

	int32 AppendComponentHulls(const TArray<FVector3f>& Vertices, const TArray<FIntVector>& Triangles, TArray<TArray<FVector>>& OutHulls, int32 MaxHulls)
	{
		const int32 NumV = Vertices.Num();
		// Connected parts: union-find over the vertices each triangle joins (vertices no triangle uses are dropped).
		TArray<int32> Parent;
		Parent.SetNumUninitialized(NumV);
		for (int32 i = 0; i < NumV; ++i)
		{
			Parent[i] = i;
		}
		auto Find = [&Parent](int32 X)
		{
			while (Parent[X] != X)
			{
				Parent[X] = Parent[Parent[X]];
				X = Parent[X];
			}
			return X;
		};
		auto Union = [&Parent, &Find](int32 X, int32 Y)
		{
			X = Find(X);
			Y = Find(Y);
			if (X != Y)
			{
				Parent[Y] = X;
			}
		};
		TArray<bool> Used;
		Used.Init(false, NumV);
		for (const FIntVector& T : Triangles)
		{
			if (!Vertices.IsValidIndex(T.X) || !Vertices.IsValidIndex(T.Y) || !Vertices.IsValidIndex(T.Z))
			{
				continue;
			}
			Used[T.X] = Used[T.Y] = Used[T.Z] = true;
			Union(T.X, T.Y);
			Union(T.X, T.Z);
		}

		struct FPart
		{
			TArray<FVector> Points;
			FBox Box = FBox(ForceInit);
			bool bAlive = true;
		};
		TArray<FPart> Parts;
		TMap<int32, int32> PartOfRoot;
		for (int32 i = 0; i < NumV; ++i)
		{
			if (!Used[i])
			{
				continue;
			}
			const int32 Root = Find(i);
			int32* Index = PartOfRoot.Find(Root);
			if (!Index)
			{
				Index = &PartOfRoot.Add(Root, Parts.Num());
				Parts.AddDefaulted();
			}
			FPart& P = Parts[*Index];
			const FVector V(Vertices[i]);
			P.Points.Add(V);
			P.Box += V;
		}
		const int32 NumParts = Parts.Num();

		// Largest first (by the bounds' diagonal, which a flat part has too).
		Parts.Sort([](const FPart& A, const FPart& B) { return A.Box.GetSize().SizeSquared() > B.Box.GetSize().SizeSquared(); });
		auto Merge = [&Parts](int32 From, int32 Into)
		{
			Parts[Into].Points.Append(MoveTemp(Parts[From].Points));
			Parts[Into].Box += Parts[From].Box;
			Parts[From].bAlive = false;
		};
		// A part inside a larger part's bounds (a centimetre of slack) joins it: details on a surface, not open space.
		for (int32 i = 1; i < Parts.Num(); ++i)
		{
			for (int32 j = 0; j < i; ++j)
			{
				if (Parts[j].bAlive && Parts[j].Box.ExpandBy(1.0).IsInsideOrOn(Parts[i].Box))
				{
					Merge(i, j);
					break;
				}
			}
		}
		// Beyond the cap the smallest parts join whichever kept part's bounds are nearest.
		TArray<int32> Kept;
		for (int32 i = 0; i < Parts.Num(); ++i)
		{
			if (Parts[i].bAlive)
			{
				if (Kept.Num() < FMath::Max(MaxHulls, 1))
				{
					Kept.Add(i);
					continue;
				}
				int32 Best = Kept[0];
				double BestD = TNumericLimits<double>::Max();
				for (const int32 k : Kept)
				{
					const double D = Parts[k].Box.ComputeSquaredDistanceToBox(Parts[i].Box);
					if (D < BestD)
					{
						BestD = D;
						Best = k;
					}
				}
				Merge(i, Best);
			}
		}
		for (const int32 k : Kept)
		{
			if (Parts[k].Points.Num() >= 3)
			{
				OutHulls.Add(HullPoints(MoveTemp(Parts[k].Points)));
			}
		}
		return NumParts;
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
		bool bMatched = true;
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
					bMatched = false; // not two triangles sharing a diagonal: the cell is not the grid's
					continue;
				}
				FDynamicMesh3::FEdgeFlipInfo Info;
				if (Mesh.FlipEdge(Edge, Info) == UE::Geometry::EMeshResult::Ok)
				{
					++Flips;
				}
				else
				{
					bMatched = false;
				}
			}
		}
		return bMatched ? Flips : -1;
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
