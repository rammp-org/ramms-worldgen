// Copyright ATDev. All Rights Reserved.
#include "RammsProcPiece.h"

#include "Components/DynamicMeshComponent.h"
#include "Components/DecalComponent.h"
#include "Components/BillboardComponent.h"
#include "UDynamicMesh.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshMaterialFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "Engine/EngineTypes.h"
#include "Materials/MaterialInterface.h"
#include "PCG/RammsGround.h"
#include "RammsCollisionShapes.h"
#include "RammsSegmentationLabels.h"
#include "RammsWorldGenModule.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Package.h"

namespace
{
	// All primitives are appended with material IDs so a piece can carry two
	// materials (trunk/foliage, ramp/warning strip, wall/frame).
	FGeometryScriptPrimitiveOptions Opts(int32 MaterialID)
	{
		FGeometryScriptPrimitiveOptions O;
		O.PolygroupMode = EGeometryScriptPrimitivePolygroupMode::PerFace;
		O.bFlipOrientation = false;
		O.UVMode = EGeometryScriptPrimitiveUVMode::Uniform;
		O.MaterialID = MaterialID;
		return O;
	}

	// --- collision capture ---------------------------------------------------------
	//
	// AppendCollisionShapes runs a kind's own builder into a scratch mesh with a capture open, and every primitive the
	// builder appends then also reports itself as a collision shape: the mesh Chaos collides with and the shapes another
	// engine collides with come from one code path, so they cannot drift apart. Boxes report themselves exactly; the
	// other convex primitives (cylinders, cones, sphere-boxes, prisms, a ramp's planar block) as the hull of the
	// vertices they appended; grids lifted onto a twisted or curved surface (curb ramp flares, mounds) as height grids
	// over their own vertices. One capture at a time, on the game thread.
	struct FShapeCapture
	{
		const UDynamicMesh* Mesh = nullptr;
		/** Piece space to world. It may carry the actor's scale, which is baked into the shapes. */
		FTransform ToWorld = FTransform::Identity;
		/** Surface per material slot (0 primary, 1 secondary, 2 glass). */
		FName Surfaces[3];
		FName Source;
		TArray<FRammsCollisionShape>* Out = nullptr;

		FName Surface(int32 Mat) const { return Surfaces[FMath::Clamp(Mat, 0, 2)]; }
		FVector Scale() const { return ToWorld.GetScale3D(); }
		FTransform Frame() const { return FTransform(ToWorld.GetRotation(), ToWorld.GetLocation()); }
	};
	FShapeCapture* GShapeCapture = nullptr;

	FShapeCapture* CaptureOf(const UDynamicMesh* M)
	{
		return GShapeCapture && GShapeCapture->Mesh == M ? GShapeCapture : nullptr;
	}

	// A box in piece space (Local: its centre and rotation) as a world shape. Scaled by a uniform positive scale it is
	// still a box; any other scale makes its corners a hull.
	void CaptureBox(FShapeCapture& C, const FTransform& Local, const FVector& Half, int32 Mat)
	{
		const FVector S = C.Scale();
		const double Su = S.GetAbsMax();
		if (S.X > 0.0 && S.Y > 0.0 && S.Z > 0.0 && S.GetAbsMin() >= Su * (1.0 - 1.0e-6))
		{
			RammsCollisionShapes::AddBox(*C.Out, FTransform(Local.GetRotation(), Local.GetLocation() * Su) * C.Frame(), Half * Su, C.Surface(Mat), C.Source);
			return;
		}
		TArray<FVector3f> Corners;
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector P((k & 1) ? Half.X : -Half.X, (k & 2) ? Half.Y : -Half.Y, (k & 4) ? Half.Z : -Half.Z);
			Corners.Add(FVector3f(Local.TransformPosition(P) * S));
		}
		RammsCollisionShapes::AddConvex(*C.Out, C.Frame(), MoveTemp(Corners), C.Surface(Mat), C.Source);
	}

	// When capturing: the hull of the vertices appended to M from Start on, one convex primitive. FloorZ also drops
	// every point straight down to that piece-space height (a disc becomes the slab under it).
	void CaptureHull(UDynamicMesh* M, int32 Start, int32 Mat, TOptional<double> FloorZ = TOptional<double>())
	{
		FShapeCapture* C = CaptureOf(M);
		if (!C || Start == INDEX_NONE)
		{
			return;
		}
		const FVector S = C->Scale();
		TArray<FVector3f> Points;
		M->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				if (Vid < Start) { continue; }
				const FVector P = Mesh.GetVertex(Vid);
				Points.Add(FVector3f(P * S));
				if (FloorZ.IsSet()) { Points.Add(FVector3f(FVector(P.X, P.Y, FloorZ.GetValue()) * S)); }
			}
		});
		if (Points.Num() >= 4)
		{
			RammsCollisionShapes::AddConvex(*C->Out, C->Frame(), MoveTemp(Points), C->Surface(Mat), C->Source);
		}
	}

	// Points in convex position (a sphere-box's lie on an ellipsoid) are their own hull only when every edge between them
	// folds outward, and the sphere-box splits some of its quads the other way, a dent of up to a few centimetres on a
	// canopy blob. Flip those edges (Lawson's flips, which end at the hull) so that the mesh Chaos collides with is the
	// hull the shapes report. A primitive already convex (a cylinder, a frustum, a prism) has none to flip. UV and normal
	// seams are left alone; the box projection and normal pass after the builders redo both anyway.
	void FoldOutward(UDynamicMesh* M, int32 Start)
	{
		M->EditMesh([&](UE::Geometry::FDynamicMesh3& Mesh)
		{
			using UE::Geometry::FDynamicMesh3;
			using UE::Geometry::FIndex2i;
			FVector Centre = FVector::ZeroVector;
			int32 Count = 0;
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				if (Vid >= Start) { Centre += Mesh.GetVertex(Vid); ++Count; }
			}
			if (Count < 4) { return; }
			Centre /= Count;
			const UE::Geometry::FDynamicMeshAttributeSet* Attributes = Mesh.HasAttributes() ? Mesh.Attributes() : nullptr;
			for (int32 Pass = 0; Pass < 32; ++Pass)
			{
				int32 Flips = 0;
				for (const int32 Eid : Mesh.EdgeIndicesItr())
				{
					const FIndex2i V = Mesh.GetEdgeV(Eid);
					if (V.A < Start || V.B < Start || Mesh.IsBoundaryEdge(Eid) || (Attributes && Attributes->IsSeamEdge(Eid))) { continue; }
					const FIndex2i O = Mesh.GetEdgeOpposingV(Eid);
					if (O.A == FDynamicMesh3::InvalidID || O.B == FDynamicMesh3::InvalidID || Mesh.FindEdge(O.A, O.B) != FDynamicMesh3::InvalidID) { continue; }
					const FVector A = Mesh.GetVertex(V.A), B = Mesh.GetVertex(V.B), C = Mesh.GetVertex(O.A), D = Mesh.GetVertex(O.B);
					const FVector Normal = FVector::CrossProduct(B - A, C - A);
					const double SideD = FVector::DotProduct(Normal, D - A);
					const double SideCentre = FVector::DotProduct(Normal, Centre - A);
					const double Scale = Normal.Size() * FMath::Max((D - A).Size(), (Centre - A).Size());
					if (Scale <= 0.0 || FMath::Abs(SideD) <= 1.0e-9 * Scale || FMath::Abs(SideCentre) <= 1.0e-9 * Scale)
					{
						continue; // flat across the edge: either split is the hull
					}
					if (SideD * SideCentre > 0.0)
					{
						continue; // D is inside the face across the edge: the edge folds outward
					}
					FDynamicMesh3::FEdgeFlipInfo Info;
					if (Mesh.FlipEdge(Eid, Info) == UE::Geometry::EMeshResult::Ok) { ++Flips; }
				}
				if (Flips == 0) { break; }
			}
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown);
	}

	// Append one convex primitive (Append adds it to M), make its mesh its hull, and report that hull when capturing.
	void Hull(UDynamicMesh* M, int32 Mat, TFunctionRef<void()> Append, TOptional<double> FloorZ = TOptional<double>())
	{
		int32 Start = 0;
		M->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh) { Start = Mesh.MaxVertexID(); });
		Append();
		FoldOutward(M, Start);
		CaptureHull(M, Start, Mat, FloorZ);
	}

	// When capturing: the solid under the upward-facing triangles appended to M from Start on, one prism per triangle
	// down to piece-space BottomZ. Exact for any surface and any scale: the fallback where a height grid cannot say it.
	void CapturePrisms(UDynamicMesh* M, int32 Start, double BottomZ, int32 Mat)
	{
		FShapeCapture* C = CaptureOf(M);
		if (!C || Start == INDEX_NONE)
		{
			return;
		}
		const FVector S = C->Scale();
		const FTransform Frame = C->Frame();
		M->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			for (const int32 Tid : Mesh.TriangleIndicesItr())
			{
				const UE::Geometry::FIndex3i T = Mesh.GetTriangle(Tid);
				if (T.A < Start || T.B < Start || T.C < Start || Mesh.GetTriNormal(Tid).Z <= 1.0e-3)
				{
					continue; // another primitive's, or a wall or the underside of the solid
				}
				RammsCollisionShapes::AddTrianglePrism(*C->Out, Frame, S, Mesh.GetVertex(T.A), Mesh.GetVertex(T.B), Mesh.GetVertex(T.C), BottomZ, C->Surface(Mat), C->Source);
			}
		});
	}

	// The grid primitive appended to M from vertex Start on, NX by NY vertices Spacing apart with sample (0, 0) at
	// piece-space Origin, its top lifted onto a surface: split its cells the HeightGrid way (RammsCollisionShapes.h),
	// so that what Chaos collides with is what the grid describes, and report it as a height grid when capturing,
	// solid down to piece-space BottomZ. A grid its mesh does not match, or one the piece's scale mirrors or turns
	// over, is reported as one prism per triangle instead (CapturePrisms): still exact, never a hole.
	void Grid(UDynamicMesh* M, int32 Start, const FVector& Origin, int32 NX, int32 NY, const FVector2D& Spacing, double BottomZ, int32 Mat)
	{
		if (Start == INDEX_NONE)
		{
			return;
		}
		TArray<float> Heights;
		int32 Flipped = -1;
		M->EditMesh([&](UE::Geometry::FDynamicMesh3& Mesh)
		{
			Flipped = RammsCollisionShapes::MatchHeightGridDiagonals(Mesh, Start, FTransform(Origin), NX, NY, Spacing, Heights);
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown);
		FShapeCapture* C = CaptureOf(M);
		if (Flipped < 0)
		{
			static bool bWarned = false;
			if (!bWarned)
			{
				bWarned = true;
				UE_LOG(LogRammsWorldGen, Warning, TEXT("ProcPiece: a %dx%d grid did not match its mesh (%s): its collision is exported as one prism per triangle, not as a height grid"),
					NX, NY, C ? *C->Source.ToString() : TEXT("built"));
			}
			CapturePrisms(M, Start, BottomZ, Mat);
			return;
		}
		if (!C)
		{
			return;
		}
		const FVector S = C->Scale();
		if (S.X <= 0.0 || S.Y <= 0.0 || S.Z <= 0.0)
		{
			// Mirrored, the grid's cells split the other way; turned over, its solid is above it: not a height grid.
			CapturePrisms(M, Start, BottomZ, Mat);
			return;
		}
		float Lowest = Heights[0];
		for (float& H : Heights)
		{
			Lowest = FMath::Min(Lowest, H);
			H *= static_cast<float>(S.Z);
		}
		FRammsCollisionShape& G = C->Out->AddDefaulted_GetRef();
		G.Type = ERammsCollisionShapeType::HeightGrid;
		G.Transform = FTransform(Origin * S) * C->Frame();
		G.NumX = NX;
		G.NumY = NY;
		G.Spacing = FVector2D(Spacing.X * S.X, Spacing.Y * S.Y);
		G.Heights = MoveTemp(Heights);
		G.BaseDepth = static_cast<float>(FMath::Max((Lowest - (BottomZ - Origin.Z)) * S.Z, 0.1));
		G.Surface = C->Surface(Mat);
		G.Source = C->Source;
	}

	// Box centred at Centre. Thin wrapper so every builder reads the same way.
	void Box(UDynamicMesh* M, const FVector& Centre, const FVector& Size, const FRotator& Rot = FRotator::ZeroRotator, int32 Mat = 0)
	{
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			M, Opts(Mat), FTransform(Rot, Centre), static_cast<float>(Size.X), static_cast<float>(Size.Y), static_cast<float>(Size.Z), 0, 0, 0,
			EGeometryScriptPrimitiveOriginMode::Center);
		if (FShapeCapture* C = CaptureOf(M))
		{
			CaptureBox(*C, FTransform(Rot, Centre), Size * 0.5, Mat);
		}
	}

	// A sloped slab: a box of plan length Run and width Width whose top surface
	// descends by Rise toward +X, starting at local X = X0 and height Z0 (the
	// top of the high end). Thickness is chosen so the low end sinks below
	// ground and the high end's underside does not float.
	void SlopedSlab(UDynamicMesh* M, double X0, double Y0, double Z0, double Run, double Width, double Rise, int32 Mat = 0)
	{
		if (Run <= KINDA_SMALL_NUMBER || Width <= KINDA_SMALL_NUMBER)
		{
			return;
		}
		const double Angle = FMath::RadiansToDegrees(FMath::Atan2(Rise, Run));
		const double Length = FMath::Sqrt(Run * Run + Rise * Rise);
		const double Thickness = FMath::Max(Rise, 2.0) + 2.0;
		// Centre of the top surface, then push down by half the thickness along the surface normal.
		const FVector TopCentre(X0 + Run * 0.5, Y0, Z0 - Rise * 0.5);
		const FRotator Rot(-Angle, 0.0, 0.0); // negative pitch: +X goes down
		const FVector Normal = Rot.RotateVector(FVector::UpVector);
		Box(M, TopCentre - Normal * (Thickness * 0.5), FVector(Length, Width, Thickness), Rot, Mat);
	}
}

ARammsProcPiece::ARammsProcPiece()
{
	PrimaryActorTick.bCanEverTick = false;
	MeshComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("Mesh"));
	SetRootComponent(MeshComponent);
	// Movable: pieces are spawned at generation time and attached to the (movable) generator.
	MeshComponent->SetMobility(EComponentMobility::Movable);
	MeshComponent->SetCollisionProfileName(TEXT("BlockAll"));
	MeshComponent->bEnableComplexCollision = true;
	MeshComponent->CollisionType = ECollisionTraceFlag::CTF_UseComplexAsSimple;
}

ERammsProcPieceKind ARammsProcPiece::GetEffectiveKind() const
{
	if (KindId >= 0 && KindId <= static_cast<int32>(ERammsProcPieceKind::Hump)) // keep in step with the last enum value
	{
		return static_cast<ERammsProcPieceKind>(KindId);
	}
	return Kind;
}

void ARammsProcPiece::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Rebuild();
}

void ARammsProcPiece::Rebuild()
{
	if (!MeshComponent)
	{
		return;
	}
	UDynamicMesh* Mesh = MeshComponent->GetDynamicMesh();
	if (!Mesh)
	{
		return;
	}
	Mesh->Reset();
	UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(Mesh);

#if WITH_EDITOR
	// Editor sprites (a decal's icon) would litter screenshots of generated streets: drop them.
	TArray<UBillboardComponent*> Sprites;
	GetComponents<UBillboardComponent>(Sprites);
	for (UBillboardComponent* B : Sprites) { if (B) { B->DestroyComponent(); } }
#endif
	if (GetEffectiveKind() == ERammsProcPieceKind::Decal)
	{
		// No geometry: a decal component projecting the material down onto whatever is below.
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComponent->NotifyMeshUpdated();
		BuildDecal();
		return;
	}
	if (DecalComponent)
	{
		DecalComponent->DestroyComponent();
		DecalComponent = nullptr;
	}

	switch (GetEffectiveKind())
	{
	case ERammsProcPieceKind::Slab:     BuildSlab(Mesh); break;
	case ERammsProcPieceKind::Curb:     BuildCurb(Mesh); break;
	case ERammsProcPieceKind::CurbRamp: BuildCurbRamp(Mesh); break;
	case ERammsProcPieceKind::Step:
	case ERammsProcPieceKind::Stairs:   BuildSteps(Mesh); break;
	case ERammsProcPieceKind::Ramp:     BuildRamp(Mesh); break;
	case ERammsProcPieceKind::Wall:     BuildWall(Mesh); break;
	case ERammsProcPieceKind::Door:     BuildDoor(Mesh); break;
	case ERammsProcPieceKind::Bollard:  BuildBollard(Mesh); break;
	case ERammsProcPieceKind::Tree:     BuildTree(Mesh); break;
	case ERammsProcPieceKind::Shrub:    BuildShrub(Mesh); break;
	case ERammsProcPieceKind::Roof:     BuildRoof(Mesh); break;
	case ERammsProcPieceKind::Planter:  BuildPlanter(Mesh); break;
	case ERammsProcPieceKind::Column:   BuildColumn(Mesh); break;
	case ERammsProcPieceKind::Mound:    BuildMound(Mesh); break;
	case ERammsProcPieceKind::Patch:    BuildPatch(Mesh); break;
	case ERammsProcPieceKind::GroundPrisms: BuildGroundPrisms(Mesh); break;
	case ERammsProcPieceKind::Hump:     BuildHump(Mesh); break;
	default: BuildSlab(Mesh); break;
	}

	// World-aligned box projection so tiling surface materials read at a
	// consistent scale on every piece regardless of its size.
	const double S = FMath::Max(UVScale, 1.0);
	FGeometryScriptMeshSelection WholeMesh;
	UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
		Mesh, 0, FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(S, S, S)), WholeMesh, 2);
	if (GetEffectiveKind() == ERammsProcPieceKind::Tree || GetEffectiveKind() == ERammsProcPieceKind::Shrub || GetEffectiveKind() == ERammsProcPieceKind::Mound)
	{
		FGeometryScriptCalculateNormalsOptions NormalOpts;
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(Mesh, NormalOpts);
	}
	else
	{
		UGeometryScriptLibrary_MeshNormalsFunctions::SetPerFaceNormals(Mesh);
	}

	ApplyMaterials();
	ApplyCollision();
}

namespace
{
	double RammsRange(FRandomStream& Rs, double Lo, double Hi) { return Lo + (Hi - Lo) * Rs.FRand(); }
}

// --- builders ------------------------------------------------------------------

void ARammsProcPiece::BuildSlab(UDynamicMesh* M)
{
	// Top face at z = 0 so the slab sits flush with whatever walks on it.
	const double T = FMath::Max(SizeZ, 1.0);
	Box(M, FVector(SizeX * 0.5, 0.0, -T * 0.5), FVector(SizeX, SizeY, T));
}

void ARammsProcPiece::BuildCurb(UDynamicMesh* M)
{
	const double H = FMath::Max(Rise, 1.0);
	const double D = FMath::Max(SizeY, 5.0);
	// Curb rises from the road level (z = -H) to the sidewalk level (z = 0),
	// with the sidewalk side at -Y and the road side at +Y.
	Box(M, FVector(SizeX * 0.5, 0.0, -H * 0.5), FVector(SizeX, D, H));
}

void ARammsProcPiece::BuildCurbRamp(UDynamicMesh* M)
{
	// Origin at the top of the ramp on the sidewalk, descending toward +X to the road at z = -Rise at
	// x = Run (the curb line). The ramp is a solid block whose top is the walking surface; it fills a cut
	// in the sidewalk slab (the street network leaves that cut), so it is the surface, not buried in it.
	// The flares either side are part of the same cut: at the top (x = 0) they are level with the walk,
	// and along the curb line they rise from road level at the ramp edge to the full curb height at
	// their outer edge, where the curb resumes.
	const double H = FMath::Max(Rise, 1.0);
	const double RunSlope = FMath::Max(Slope, 0.01);
	const double Run = H / RunSlope;
	const double W = FMath::Max(SizeY, 60.0);
	const double Bottom = -H - 2.0; // 2 cm into the road

	// One block per strip, its top vertices then lifted onto the surface height h(x, y). StepsX and StepsY are vertex
	// counts along each side. Returns the strip's first vertex (INDEX_NONE when too narrow to build).
	const int32 StepsX = FMath::Clamp(FMath::RoundToInt(Run / 30.0), 2, 16);
	auto Strip = [&](double Y0, double Y1, int32 StepsY, TFunctionRef<double(double X, double Y)> Height) -> int32
	{
		if (Y1 - Y0 < 1.0) { return INDEX_NONE; }
		int32 Start = 0;
		M->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh) { Start = Mesh.MaxVertexID(); });
		UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
			M, Opts(0), FTransform(FVector(Run * 0.5, (Y0 + Y1) * 0.5, Bottom * 0.5)), static_cast<float>(Run), static_cast<float>(Y1 - Y0), static_cast<float>(-Bottom),
			StepsX, StepsY, 0, EGeometryScriptPrimitiveOriginMode::Center);
		M->EditMesh([&](UE::Geometry::FDynamicMesh3& Mesh)
		{
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				if (Vid < Start) { continue; }
				FVector3d P = Mesh.GetVertex(Vid);
				if (P.Z > Bottom * 0.5)
				{
					P.Z = Height(FMath::Clamp(P.X, 0.0, Run), P.Y);
					Mesh.SetVertex(Vid, P);
				}
			}
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown);
		return Start;
	};
	// The ramp itself has a plane for a top: one convex block.
	CaptureHull(M, Strip(-W * 0.5, W * 0.5, 2, [&](double X, double) { return -H * X / Run; }), 0);

	const double FlareRun = H / FMath::Max(FlareSlope, 0.01); // sideways extent of each flare
	if (FlareRun > 1.0)
	{
		const int32 Steps = FMath::Clamp(FMath::RoundToInt(FlareRun / 30.0), 2, 12);
		for (int32 Side = -1; Side <= 1; Side += 2)
		{
			const double Y0 = Side < 0 ? -W * 0.5 - FlareRun : W * 0.5;
			const int32 Start = Strip(Y0, Y0 + FlareRun, Steps, [&](double X, double Y)
			{
				const double T = FMath::Clamp((FMath::Abs(Y) - W * 0.5) / FlareRun, 0.0, 1.0); // 0 at the ramp edge, 1 at the outer edge
				return -H * (X / Run) * (1.0 - T);
			});
			// A flare's top is twisted (level along the walk and its outer edge, down to the road at the ramp's foot), so
			// it is not convex: a height grid over the strip's own vertices, cells split the way the grid says.
			Grid(M, Start, FVector(0.0, Y0, 0.0), StepsX, Steps, FVector2D(Run / (StepsX - 1), FlareRun / (Steps - 1)), Bottom, 0);
		}
	}

	if (WarningDepth > 1.0)
	{
		// Detectable warning strip across the bottom of the ramp, slightly proud of the surface.
		const double D = FMath::Min(WarningDepth, Run);
		const double Angle = FMath::RadiansToDegrees(FMath::Atan2(H, Run));
		const FRotator Rot(-Angle, 0.0, 0.0);
		const FVector Normal = Rot.RotateVector(FVector::UpVector);
		const double XC = Run - D * 0.5;
		const FVector Centre(XC, 0.0, -H * (XC / Run));
		// 4 mm proud of the ramp (truncated domes are about 5 mm): a wheelchair rolls over it.
		Box(M, Centre + Normal * 0.1, FVector(D, W, 0.6), Rot, 1);
	}
}

void ARammsProcPiece::BuildSteps(UDynamicMesh* M)
{
	// Steps rise toward -X: the bottom step starts at the origin and the top
	// landing is at x = -StepCount * tread. SizeX is the tread depth.
	const int32 N = FMath::Max(StepCount, 1);
	const double Tread = FMath::Max(SizeX, 20.0);
	const double R = FMath::Max(Rise, 1.0);
	const double W = FMath::Max(SizeY, 60.0);
	for (int32 i = 0; i < N; ++i)
	{
		const double Height = R * (i + 1);
		const double X = -Tread * (i + 0.5);
		Box(M, FVector(X, 0.0, Height * 0.5), FVector(Tread, W, Height));
	}
}

void ARammsProcPiece::BuildRamp(UDynamicMesh* M)
{
	// Ramp rises toward -X from the origin; landing continues past the top.
	const double H = FMath::Max(Rise, 1.0);
	const double RunSlope = FMath::Max(Slope, 0.01);
	const double Run = H / RunSlope;
	const double W = FMath::Max(SizeY, 60.0);
	// Reuse the descending builder mirrored: high end at x = -Run, low end at origin.
	SlopedSlab(M, -Run, 0.0, H, Run, W, H, 0);
	if (LandingSize > 1.0)
	{
		Box(M, FVector(-Run - LandingSize * 0.5, 0.0, H * 0.5), FVector(LandingSize, W, H));
	}
	// Edge protection: low curbs along both sides (ADA 405.9).
	const double Lip = 8.0;
	for (int32 Side = -1; Side <= 1; Side += 2)
	{
		Box(M, FVector(-(Run + LandingSize) * 0.5, Side * (W * 0.5 + 3.0), (H + Lip) * 0.5), FVector(Run + LandingSize, 6.0, H + Lip), FRotator::ZeroRotator, 1);
	}
}

void ARammsProcPiece::BuildWall(UDynamicMesh* M)
{
	const double L = FMath::Max(SizeX, 10.0);
	const double T = FMath::Max(SizeY, 5.0);
	const double H = FMath::Max(SizeZ, 50.0);
	// Openings: the door (OpeningWidth) plus any windows listed in Openings, as [centre, width, sill, head].
	struct FOpening { double C, W, Sill, Head; };
	TArray<FOpening> Ops;
	if (OpeningWidth > 0.0 && OpeningWidth < L)
	{
		const double OW = OpeningWidth;
		Ops.Add({ FMath::Clamp(OpeningOffset > 0.0 ? OpeningOffset : L * 0.5, OW * 0.5, L - OW * 0.5), OW, 0.0, FMath::Clamp(OpeningHeight, 100.0, H) });
	}
	TArray<FString> Items;
	Openings.ParseIntoArray(Items, TEXT(";"), true);
	for (const FString& It : Items)
	{
		TArray<FString> V;
		It.ParseIntoArray(V, TEXT(","), true);
		if (V.Num() < 4) { continue; }
		const double C = FCString::Atod(*V[0]), W = FCString::Atod(*V[1]), Sill = FCString::Atod(*V[2]), Head = FCString::Atod(*V[3]);
		if (W < 20.0 || C - W * 0.5 < 5.0 || C + W * 0.5 > L - 5.0 || Head <= Sill + 20.0 || Head > H) { continue; }
		Ops.Add({ C, W, FMath::Max(Sill, 0.0), Head });
	}
	Ops.Sort([](const FOpening& A, const FOpening& B) { return A.C < B.C; });
	// Drop openings that would overlap the one before them.
	for (int32 i = Ops.Num() - 1; i > 0; --i)
	{
		if (Ops[i].C - Ops[i].W * 0.5 < Ops[i - 1].C + Ops[i - 1].W * 0.5 + 20.0) { Ops.RemoveAt(i); }
	}
	// Solid wall between openings, and the parts below / above each opening.
	double X = 0.0;
	for (const FOpening& O : Ops)
	{
		const double A = O.C - O.W * 0.5, B = O.C + O.W * 0.5;
		if (A - X > 0.5) { Box(M, FVector((X + A) * 0.5, 0.0, H * 0.5), FVector(A - X, T, H)); }
		if (O.Sill > 0.5) { Box(M, FVector(O.C, 0.0, O.Sill * 0.5), FVector(O.W, T, O.Sill)); }
		if (H - O.Head > 0.5) { Box(M, FVector(O.C, 0.0, O.Head + (H - O.Head) * 0.5), FVector(O.W, T, H - O.Head)); }
		X = B;
	}
	if (L - X > 0.5) { Box(M, FVector((X + L) * 0.5, 0.0, H * 0.5), FVector(L - X, T, H)); }
	// Frames in the secondary material, proud of both faces by 2 cm so they never share a plane with the
	// wall or with the 1 cm finish liners rooms put on it (no z-fighting): casing jambs and a head for doors,
	// a full frame, a sill ledge and a glass pane (third material) for windows.
	const double Casing = 7.0, Proud = T + 4.0;
	for (const FOpening& O : Ops)
	{
		const double A = O.C - O.W * 0.5, B = O.C + O.W * 0.5;
		const double Hgt = O.Head - O.Sill;
		Box(M, FVector(A - Casing * 0.5, 0.0, O.Sill + Hgt * 0.5), FVector(Casing, Proud, Hgt), FRotator::ZeroRotator, 1);
		Box(M, FVector(B + Casing * 0.5, 0.0, O.Sill + Hgt * 0.5), FVector(Casing, Proud, Hgt), FRotator::ZeroRotator, 1);
		Box(M, FVector(O.C, 0.0, O.Head + Casing * 0.5), FVector(O.W + 2.0 * Casing, Proud, Casing), FRotator::ZeroRotator, 1);
		if (O.Sill > 0.5)
		{
			Box(M, FVector(O.C, 0.0, O.Sill - 2.0), FVector(O.W + 2.0 * Casing + 6.0, T + 10.0, 4.0), FRotator::ZeroRotator, 1);
			// Mullion and glass.
			Box(M, FVector(O.C, 0.0, O.Sill + Hgt * 0.5), FVector(4.0, 6.0, Hgt), FRotator::ZeroRotator, 1);
			Box(M, FVector(O.C, 0.0, O.Sill + Hgt * 0.5), FVector(O.W, 1.0, Hgt), FRotator::ZeroRotator, 2);
		}
	}
}

void ARammsProcPiece::BuildDoor(UDynamicMesh* M)
{
	// Door leaf standing in its opening, hinge at the origin, extending along +X.
	const double W = FMath::Max(SizeX, 40.0);
	const double T = FMath::Clamp(SizeY, 2.0, 10.0);
	const double H = FMath::Max(SizeZ, 150.0);
	Box(M, FVector(W * 0.5, 0.0, Rise + H * 0.5), FVector(W, T, H));
	// Lever handles on both faces near the latch edge, in the secondary material.
	for (int32 Side = -1; Side <= 1; Side += 2)
	{
		Box(M, FVector(W - 7.0, Side * (T * 0.5 + 1.0), Rise + 100.0), FVector(6.0, 2.0, 6.0), FRotator::ZeroRotator, 1);
		Box(M, FVector(W - 13.0, Side * (T * 0.5 + 4.0), Rise + 100.0), FVector(14.0, 2.0, 2.0), FRotator::ZeroRotator, 1);
	}
	if (Rise > 0.05)
	{
		// Raised threshold across the opening; this is the part that matters.
		Box(M, FVector(W * 0.5, 0.0, Rise * 0.5), FVector(W, FMath::Max(T * 3.0, 12.0), Rise), FRotator::ZeroRotator, 1);
	}
}

void ARammsProcPiece::BuildBollard(UDynamicMesh* M)
{
	const double R = FMath::Max(SizeX, 6.0) * 0.5;
	const double H = FMath::Max(SizeZ, 30.0);
	Hull(M, 0, [&]() { UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(M, Opts(0), FTransform(FVector::ZeroVector), static_cast<float>(R), static_cast<float>(H), 16, 0, true, EGeometryScriptPrimitiveOriginMode::Base); });
}

void ARammsProcPiece::BuildTree(UDynamicMesh* M)
{
	// A tapered trunk, two stub branches and a canopy of overlapping spheres; the per-instance
	// variation comes from a hash of the piece's own dimensions and name, so a seed reproduces it.
	const double Canopy = FMath::Max(SizeX, 100.0);
	const double H = FMath::Max(SizeZ, 200.0);
	FRandomStream Rs(static_cast<int32>(HashCombine(GetTypeHash(GetFName()), GetTypeHash(FMath::RoundToInt(Canopy * 7.0 + H)))));
	const double TrunkR = Canopy * RammsRange(Rs, 0.045, 0.065);
	const double TrunkH = H * RammsRange(Rs, 0.38, 0.5);
	// Each part is convex (a frustum, a sphere-box), so each part's collision is the hull of its own vertices.
	Hull(M, 1, [&]() { UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCone(M, Opts(1), FTransform(FVector::ZeroVector), static_cast<float>(TrunkR), static_cast<float>(TrunkR * 0.55), static_cast<float>(TrunkH * 1.15), 10, 1, true, EGeometryScriptPrimitiveOriginMode::Base); });
	for (int32 b = 0; b < 2; ++b)
	{
		const double Yaw = RammsRange(Rs, 0.0, 360.0), Tilt = RammsRange(Rs, 35.0, 55.0);
		const FTransform Bx(FRotator(Tilt, Yaw, 0.0), FVector(0.0, 0.0, TrunkH * RammsRange(Rs, 0.7, 0.95)));
		Hull(M, 1, [&]() { UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCone(M, Opts(1), Bx, static_cast<float>(TrunkR * 0.5), static_cast<float>(TrunkR * 0.2), static_cast<float>(Canopy * 0.35), 7, 1, true, EGeometryScriptPrimitiveOriginMode::Base); });
	}
	const double CanopyH = H - TrunkH;
	const double Rc = Canopy * 0.5;
	const double Zc = TrunkH + CanopyH * 0.5;
	auto Blob = [&](const FVector& Centre, double R, double ZScale)
	{
		const FTransform Xf(FRotator(0.0, RammsRange(Rs, 0.0, 90.0), 0.0), Centre, FVector(1.0, 1.0, ZScale));
		Hull(M, 0, [&]() { UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereBox(M, Opts(0), Xf, static_cast<float>(R), 5, 5, 5, EGeometryScriptPrimitiveOriginMode::Center); });
	};
	Blob(FVector(0.0, 0.0, Zc), Rc * 0.95, CanopyH / Canopy * 0.95);
	const int32 Satellites = 4 + (Rs.FRand() < 0.5 ? 1 : 0);
	for (int32 i = 0; i < Satellites; ++i)
	{
		const double A = FMath::DegreesToRadians(i * 360.0 / Satellites + RammsRange(Rs, -25.0, 25.0));
		const double D = Rc * RammsRange(Rs, 0.35, 0.6);
		const double R = Rc * RammsRange(Rs, 0.5, 0.7);
		Blob(FVector(FMath::Cos(A) * D, FMath::Sin(A) * D, Zc + RammsRange(Rs, -CanopyH * 0.25, CanopyH * 0.15)), R, RammsRange(Rs, 0.8, 1.1));
	}
	Blob(FVector(RammsRange(Rs, -Rc * 0.15, Rc * 0.15), RammsRange(Rs, -Rc * 0.15, Rc * 0.15), Zc + CanopyH * 0.3), Rc * 0.55, 0.9);
}

void ARammsProcPiece::BuildRoof(UDynamicMesh* M)
{
	// Gable: a triangle (span across local X, rise up local Y) extruded along the ridge. A roll of -90
	// degrees maps the polygon's Y to world +Z and its extrusion axis to world -Y (UE's rotation matrix:
	// roll R sends Y to (0, cosR, -sinR)), so the ridge runs along the piece's Y axis; the actor's yaw turns it.
	const double Span = FMath::Max(SizeX, 100.0), Ridge = FMath::Max(SizeY, 100.0), RoofRise = FMath::Max(SizeZ, 30.0);
	TArray<FVector2D> Tri = { FVector2D(-Span * 0.5, 0.0), FVector2D(Span * 0.5, 0.0), FVector2D(0.0, RoofRise) };
	const FTransform Xf(FRotator(0.0, 0.0, -90.0), FVector(0.0, Ridge * 0.5, 0.0));
	Hull(M, 0, [&]() { UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSimpleExtrudePolygon(M, Opts(0), Xf, Tri, static_cast<float>(Ridge), 0, true, EGeometryScriptPrimitiveOriginMode::Base); });
	// A thin soffit slab under the eaves closes the gap to the walls and reads as the roof's edge.
	Box(M, FVector(0.0, 0.0, -4.0), FVector(Span, Ridge, 8.0));
}

void ARammsProcPiece::BuildShrub(UDynamicMesh* M)
{
	const double D = FMath::Max(SizeX, 40.0);
	const double H = FMath::Max(SizeZ, 30.0);
	FRandomStream Rs(static_cast<int32>(HashCombine(GetTypeHash(GetFName()), GetTypeHash(FMath::RoundToInt(D * 3.0 + H)))));
	const FTransform Xf(FRotator::ZeroRotator, FVector(0.0, 0.0, H * 0.5), FVector(1.0, 1.0, H / D));
	Hull(M, 0, [&]() { UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereBox(M, Opts(0), Xf, static_cast<float>(D * 0.5), 5, 5, 5, EGeometryScriptPrimitiveOriginMode::Center); });
	for (int32 i = 0; i < 3; ++i)
	{
		const double A = FMath::DegreesToRadians(i * 120.0 + RammsRange(Rs, -30.0, 30.0));
		const double R = D * RammsRange(Rs, 0.28, 0.38);
		const FTransform Bx(FRotator::ZeroRotator, FVector(FMath::Cos(A) * D * 0.3, FMath::Sin(A) * D * 0.3, H * RammsRange(Rs, 0.35, 0.5)), FVector(1.0, 1.0, H / D));
		Hull(M, 0, [&]() { UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereBox(M, Opts(0), Bx, static_cast<float>(R), 4, 4, 4, EGeometryScriptPrimitiveOriginMode::Center); });
	}
}

void ARammsProcPiece::BuildPlanter(UDynamicMesh* M)
{
	const double L = FMath::Max(SizeX, 30.0), W = FMath::Max(SizeY, 30.0), H = FMath::Max(SizeZ, 20.0);
	const double T = 6.0;
	Box(M, FVector(L * 0.5, 0.0, H * 0.5), FVector(L, W, H));
	// Soil as a second-material cap just below the rim.
	Box(M, FVector(L * 0.5, 0.0, H - 2.0), FVector(L - T, W - T, 2.0), FRotator::ZeroRotator, 1);
}

void ARammsProcPiece::BuildColumn(UDynamicMesh* M)
{
	const double S = FMath::Max(SizeX, 10.0), H = FMath::Max(SizeZ, 50.0);
	Box(M, FVector(0.0, 0.0, H * 0.5), FVector(S, S, H));
}

void ARammsProcPiece::BuildMound(UDynamicMesh* M)
{
	// A grid over the ellipse's bounding box, lifted by a cosine bump: smooth to the rim, where it meets
	// the ground tangentially (the rim and the corners sit 2 cm under it so the lawn shows there).
	const double SX = FMath::Max(SizeX, 100.0), SY = FMath::Max(SizeY, 100.0), Hm = Rise;
	const int32 NX = FMath::Clamp(FMath::RoundToInt(SX / 40.0), 8, 64), NY = FMath::Clamp(FMath::RoundToInt(SY / 40.0), 8, 64);
	int32 Start = 0;
	M->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh) { Start = Mesh.MaxVertexID(); });
	// NX by NY vertices (the rectangle's step counts are vertex counts).
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendRectangleXY(M, Opts(0), FTransform::Identity, static_cast<float>(SX), static_cast<float>(SY), NX, NY);
	M->EditMesh([&](UE::Geometry::FDynamicMesh3& Mesh)
	{
		for (const int32 Vid : Mesh.VertexIndicesItr())
		{
			if (Vid < Start) { continue; }
			FVector3d P = Mesh.GetVertex(Vid);
			const double E = FMath::Sqrt(FMath::Square(2.0 * P.X / SX) + FMath::Square(2.0 * P.Y / SY));
			P.Z = (E < 1.0 ? Hm * 0.5 * (1.0 + FMath::Cos(PI * E)) : 0.0) - 2.0;
			Mesh.SetVertex(Vid, P);
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown);
	// A sheet, not a solid: its collision is a height grid over the same vertices, solid 20 cm under its lowest point
	// (the ground slab it sits on fills the rest).
	Grid(M, Start, FVector(-SX * 0.5, -SY * 0.5, 0.0), NX, NY, FVector2D(SX / (NX - 1), SY / (NY - 1)), FMath::Min(Hm, 0.0) - 22.0, 0);
}

void ARammsProcPiece::BuildPatch(UDynamicMesh* M)
{
	const double SX = FMath::Max(SizeX, 20.0), SY = FMath::Max(SizeY, 20.0);
	const FTransform Xf(FRotator::ZeroRotator, FVector(0.0, 0.0, FMath::Max(SizeZ, 0.2)), FVector(SX / 100.0, SY / 100.0, 1.0));
	// A disc floating SizeZ over the ground: its collision is the slab from the ground up to it, the same top.
	Hull(M, 0, [&]() { UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendDisc(M, Opts(0), Xf, 50.0f, 40, 1); }, 0.0);
}

// --- materials and collision ------------------------------------------------------

void ARammsProcPiece::BuildDecal()
{
	if (!DecalComponent)
	{
		DecalComponent = NewObject<UDecalComponent>(this, TEXT("Decal"));
#if WITH_EDITORONLY_DATA
		DecalComponent->bVisualizeComponent = false; // no editor sprite on every crack
#endif
		DecalComponent->SetupAttachment(MeshComponent);
		DecalComponent->RegisterComponent();
		AddInstanceComponent(DecalComponent);
	}
	// Decal X is the projection axis: pitch it straight down. Size is half extents (depth, width, length).
	DecalComponent->SetRelativeRotation(FRotator(-90.0, 0.0, 0.0));
	DecalComponent->SetRelativeLocation(FVector::ZeroVector);
	DecalComponent->DecalSize = FVector(FMath::Max(SizeZ, 2.0) * 0.5, FMath::Max(SizeY, 5.0) * 0.5, FMath::Max(SizeX, 5.0) * 0.5);
	DecalComponent->SetFadeScreenSize(0.0005f);
	UMaterialInterface* Mat = MaterialPath.IsEmpty() ? nullptr : Cast<UMaterialInterface>(FSoftObjectPath(MaterialPath).TryLoad());
	DecalComponent->SetDecalMaterial(Mat);
	DecalComponent->MarkRenderStateDirty();
}

void ARammsProcPiece::ApplyMaterials()
{
	auto Load = [](const FString& Path) -> UMaterialInterface*
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		UMaterialInterface* Mat = Cast<UMaterialInterface>(FSoftObjectPath(Path).TryLoad());
		if (!Mat)
		{
			UE_LOG(LogRammsWorldGen, Warning, TEXT("ProcPiece: material '%s' could not be loaded"), *Path);
		}
		return Mat;
	};
	UMaterialInterface* M0 = Load(MaterialPath);
	UMaterialInterface* M1 = Load(MaterialPath2);
	UMaterialInterface* M2 = Load(MaterialPath3);
	if (!M1)
	{
		M1 = M0;
	}
	if (!M2)
	{
		M2 = M1;
	}
	MeshComponent->SetNumMaterials(3);
	MeshComponent->SetMaterial(0, M0);
	MeshComponent->SetMaterial(1, M1);
	MeshComponent->SetMaterial(2, M2);
	MeshComponent->NotifyMeshUpdated();
}

void ARammsProcPiece::ApplyCollision()
{
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	MeshComponent->SetComplexAsSimpleCollisionEnabled(true, false);
	MeshComponent->UpdateCollision(false);
}

namespace
{
	// The surface a piece's primary material stands for: its segmentation label (seg:<Class> on its mesh or its actor,
	// URammsSegmentationLabels) where that names one, else its kind's usual surface.
	FName PrimarySurface(const ARammsProcPiece& Piece)
	{
		auto FromTags = [](const TArray<FName>& Tags) -> FName
		{
			for (const FName& T : Tags)
			{
				const FString S = T.ToString();
				if (S.StartsWith(URammsSegmentationLabels::TagPrefix, ESearchCase::IgnoreCase))
				{
					return RammsCollisionShapes::SurfaceForClass(FName(*S.RightChop(URammsSegmentationLabels::TagPrefix.Len())));
				}
			}
			return NAME_None;
		};
		FName Surface = Piece.MeshComponent ? FromTags(Piece.MeshComponent->ComponentTags) : NAME_None;
		if (Surface.IsNone())
		{
			Surface = FromTags(Piece.Tags);
		}
		if (!Surface.IsNone())
		{
			return Surface;
		}
		switch (Piece.GetEffectiveKind())
		{
		case ERammsProcPieceKind::Curb:     return FName(TEXT("Curb"));
		case ERammsProcPieceKind::Wall:     return FName(TEXT("Brick"));
		case ERammsProcPieceKind::Door:     return FName(TEXT("Wood"));
		case ERammsProcPieceKind::Bollard:  return FName(TEXT("Metal"));
		case ERammsProcPieceKind::Mound:    return FName(TEXT("Grass"));
		case ERammsProcPieceKind::Patch:    return FName(TEXT("Dirt"));
		case ERammsProcPieceKind::Roof:     return FName(TEXT("Default"));
		default:                            return FName(TEXT("Concrete"));
		}
	}
}

void ARammsProcPiece::AppendCollisionShapes(TArray<FRammsCollisionShape>& Out) const
{
	if (!MeshComponent || !CollisionEnabledHasPhysics(MeshComponent->GetCollisionEnabled()))
	{
		return; // nothing a body can stand on (a decal has no collision at all)
	}

	// The kinds whose builder reports its own primitives (FShapeCapture): run the builder into a scratch mesh with a
	// capture open. The builders only read the properties and write the mesh they are given, hence the const_cast.
	auto Capture = [&](void (ARammsProcPiece::*Build)(UDynamicMesh*))
	{
		UDynamicMesh* Scratch = NewObject<UDynamicMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(Scratch);
		const ERammsProcPieceKind K = GetEffectiveKind();
		FShapeCapture C;
		C.Mesh = Scratch;
		C.ToWorld = MeshComponent->GetComponentTransform();
		C.Source = GetFName();
		C.Out = &Out;
		C.Surfaces[0] = PrimarySurface(*this);
		C.Surfaces[1] = C.Surfaces[0];
		switch (K)
		{
		case ERammsProcPieceKind::Tree:  C.Surfaces[0] = FName(TEXT("Foliage")); C.Surfaces[1] = FName(TEXT("Wood")); break; // canopy, trunk
		case ERammsProcPieceKind::Shrub: C.Surfaces[0] = FName(TEXT("Foliage")); C.Surfaces[1] = C.Surfaces[0]; break;
		case ERammsProcPieceKind::Wall:  C.Surfaces[1] = FName(TEXT("Wood")); break;   // frames
		case ERammsProcPieceKind::Door:  C.Surfaces[1] = FName(TEXT("Metal")); break;  // handles, threshold
		case ERammsProcPieceKind::Planter: C.Surfaces[1] = FName(TEXT("Dirt")); break; // soil
		default: break;
		}
		C.Surfaces[2] = K == ERammsProcPieceKind::Wall ? FName(TEXT("Glass")) : C.Surfaces[1];
		{
			TGuardValue<FShapeCapture*> Open(GShapeCapture, &C);
			(const_cast<ARammsProcPiece*>(this)->*Build)(Scratch);
		}
		Scratch->MarkAsGarbage();
	};

	switch (GetEffectiveKind())
	{
	case ERammsProcPieceKind::Slab:     Capture(&ARammsProcPiece::BuildSlab); break;
	case ERammsProcPieceKind::Curb:     Capture(&ARammsProcPiece::BuildCurb); break;
	case ERammsProcPieceKind::CurbRamp: Capture(&ARammsProcPiece::BuildCurbRamp); break;
	case ERammsProcPieceKind::Step:
	case ERammsProcPieceKind::Stairs:   Capture(&ARammsProcPiece::BuildSteps); break;
	case ERammsProcPieceKind::Ramp:     Capture(&ARammsProcPiece::BuildRamp); break;
	case ERammsProcPieceKind::Wall:     Capture(&ARammsProcPiece::BuildWall); break;
	case ERammsProcPieceKind::Door:     Capture(&ARammsProcPiece::BuildDoor); break;
	case ERammsProcPieceKind::Bollard:  Capture(&ARammsProcPiece::BuildBollard); break;
	case ERammsProcPieceKind::Tree:     Capture(&ARammsProcPiece::BuildTree); break;
	case ERammsProcPieceKind::Shrub:    Capture(&ARammsProcPiece::BuildShrub); break;
	case ERammsProcPieceKind::Roof:     Capture(&ARammsProcPiece::BuildRoof); break;
	case ERammsProcPieceKind::Planter:  Capture(&ARammsProcPiece::BuildPlanter); break;
	case ERammsProcPieceKind::Column:   Capture(&ARammsProcPiece::BuildColumn); break;
	case ERammsProcPieceKind::Mound:    Capture(&ARammsProcPiece::BuildMound); break;
	case ERammsProcPieceKind::Patch:    Capture(&ARammsProcPiece::BuildPatch); break;
	case ERammsProcPieceKind::Decal:    break; // projected onto whatever is below: no collision of its own
	// The ground kinds describe themselves (RammsProcPieceGround.cpp).
	case ERammsProcPieceKind::GroundPrisms: AppendGroundPrismShapes(Out); break;
	case ERammsProcPieceKind::Hump:         AppendHumpShapes(Out); break;
	default:
		// Each kind added later reports its own shapes here, one case each; a kind with no case adds nothing.
		break;
	}
}

FRammsManifestFeature ARammsProcPiece::ToManifestFeature() const
{
	FRammsManifestFeature F;
	F.Kind = ManifestKind.IsNone() ? FName(*UEnum::GetValueAsString(GetEffectiveKind()).RightChop(FString(TEXT("ERammsProcPieceKind::")).Len())) : ManifestKind;
	F.Subtype = ManifestSubtype;
	F.Location = GetActorLocation();
	F.Rotation = GetActorRotation();
	F.Size = FVector(SizeX, SizeY, SizeZ);
	if (GetEffectiveKind() == ERammsProcPieceKind::CurbRamp)
	{
		// The whole cut: the ramp's run, its width with both flares, and the curb height; centred on the ramp.
		const double H = FMath::Max(Rise, 1.0);
		const double Run = H / FMath::Max(Slope, 0.01);
		const double FlareRun = FlareSlope < 50.0 ? H / FMath::Max(FlareSlope, 0.01) : 0.0;
		F.Size = FVector(Run, FMath::Max(SizeY, 60.0) + 2.0 * FlareRun, H);
		F.Location = GetActorTransform().TransformPosition(FVector(Run * 0.5, 0.0, -H * 0.5));
	}
	else if (GetEffectiveKind() == ERammsProcPieceKind::GroundPrisms)
	{
		// Graded ground's features (potholes, covers, cracks, pavers, curb ramps on a grade): the box of their prisms'
		// plan in the piece's frame, centred on it; its height the feature's (Rise: a curb ramp's curb) when given.
		TArray<RammsGround::FPart> Ps;
		if (RammsGround::ParseParts(Parts, Ps))
		{
			FBox2D Plan(ForceInit);
			for (const RammsGround::FPart& P : Ps) { for (const FVector& V : P.Top) { Plan += FVector2D(V.X, V.Y); } }
			const FVector2D C = Plan.GetCenter(), Sz = Plan.GetSize();
			F.Size = FVector(Sz.X, Sz.Y, Rise > 0.0 ? Rise : SizeZ);
			F.Location = GetActorTransform().TransformPosition(FVector(C.X, C.Y, Rise > 0.0 ? -Rise * 0.5 : 0.0));
		}
	}
	F.bCompliant = bCompliant;
	F.Issue = static_cast<ERammsComplianceIssue>(FMath::Clamp(IssueId, 0, static_cast<int32>(ERammsComplianceIssue::ObstacleInRoute)));
	if (Measure != 0.0)
	{
		F.Measure = Measure;
	}
	else
	{
		switch (GetEffectiveKind())
		{
		case ERammsProcPieceKind::CurbRamp:
		case ERammsProcPieceKind::Ramp:   F.Measure = Slope; break;
		case ERammsProcPieceKind::Door:   F.Measure = SizeX; break;
		case ERammsProcPieceKind::Step:
		case ERammsProcPieceKind::Stairs: F.Measure = Rise * FMath::Max(StepCount, 1); break;
		default: F.Measure = SizeY; break;
		}
	}
	F.Threshold = Threshold;
	return F;
}
