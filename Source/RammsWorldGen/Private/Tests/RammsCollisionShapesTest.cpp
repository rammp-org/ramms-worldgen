// Copyright ATDev. All Rights Reserved.

// The collision shapes a generated world exports must be the collision Chaos sees: for every piece kind (and a
// pitched, rolled piece, as on a graded street) the shapes' union bounds match the built mesh's within 1 mm, and
// vertical probes find the same top within 2 mm. The instanced-mesh half is checked the same way against Chaos's own
// simple collision of scaled, rotated instances.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CollisionQueryParams.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Math/RandomStream.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodyInstance.h"
#include "RammsCollisionShapes.h"
#include "RammsProcPiece.h"
#include "RammsWorldGenerator.h"
#include "UDynamicMesh.h"

namespace RammsCollisionShapesTest
{
	/** A throwaway game world (with a physics scene, so traces work), torn down on scope exit. */
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

		template <typename T>
		T* Spawn(const FTransform& Xf)
		{
			FActorSpawnParameters SP;
			SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			return World->SpawnActor<T>(T::StaticClass(), Xf, SP);
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

	struct FCase
	{
		FString Name;
		ERammsProcPieceKind Kind = ERammsProcPieceKind::Slab;
		TFunction<void(ARammsProcPiece&)> Setup;
		FRotator Rotation = FRotator::ZeroRotator;
		/** Probe tops agree within this (cm). */
		double Tolerance = 0.2;
		/** Share of probes allowed to hit on one side and miss on the other (they graze an edge). */
		double MissFraction = 0.02;
		/** The mesh is a closed solid, so its bottom is the shapes' bottom too (a sheet's is not). */
		bool bClosedMesh = true;
		/** Shapes expected of each type, or -1 to not check. */
		int32 Boxes = -1, Convex = -1, Grids = -1;
	};

	FBox MeshBounds(const ARammsProcPiece& Piece)
	{
		FBox B(ForceInit);
		const FTransform Xf = Piece.MeshComponent->GetComponentTransform();
		Piece.MeshComponent->GetDynamicMesh()->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				B += Xf.TransformPosition(Mesh.GetVertex(Vid));
			}
		});
		return B;
	}

	/** The mesh's bounds in the piece's own frame: probes are drawn there, so a thin piece turned 45 degrees is not
	 *  probed mostly beside it. */
	FBox LocalMeshBounds(const ARammsProcPiece& Piece)
	{
		FBox B(ForceInit);
		Piece.MeshComponent->GetDynamicMesh()->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
		{
			for (const int32 Vid : Mesh.VertexIndicesItr())
			{
				B += Mesh.GetVertex(Vid);
			}
		});
		return B;
	}

	FBox ShapeBounds(const TArray<FRammsCollisionShape>& Shapes)
	{
		FBox B(ForceInit);
		for (const FRammsCollisionShape& S : Shapes)
		{
			B += RammsCollisionShapes::GetBounds(S);
		}
		return B;
	}

	/** Bounds of the shapes' upper surfaces: a height grid's samples, without the solid it stands on. A sheet mesh (a
	 *  mound) is compared with this: the grid's walls are square to the grid, so under a tilted grid its base reaches
	 *  out past the sheet's edge, below the ground the sheet's rim is buried in. */
	FBox SurfaceBounds(const TArray<FRammsCollisionShape>& Shapes)
	{
		FBox B(ForceInit);
		for (const FRammsCollisionShape& S : Shapes)
		{
			if (S.Type != ERammsCollisionShapeType::HeightGrid)
			{
				B += RammsCollisionShapes::GetBounds(S);
				continue;
			}
			for (int32 j = 0; j < S.NumY; ++j)
			{
				for (int32 i = 0; i < S.NumX; ++i)
				{
					B += S.Transform.TransformPosition(FVector(i * S.Spacing.X, j * S.Spacing.Y, S.Heights[j * S.NumX + i]));
				}
			}
		}
		return B;
	}

	TOptional<double> ShapesTop(const TArray<FRammsCollisionShape>& Shapes, double X, double Y)
	{
		TOptional<double> Best;
		for (const FRammsCollisionShape& S : Shapes)
		{
			const TOptional<double> Z = RammsCollisionShapes::TopAt(S, X, Y);
			if (Z.IsSet() && (!Best.IsSet() || Z.GetValue() > Best.GetValue()))
			{
				Best = Z;
			}
		}
		return Best;
	}

	int32 CountOf(const TArray<FRammsCollisionShape>& Shapes, ERammsCollisionShapeType Type)
	{
		int32 N = 0;
		for (const FRammsCollisionShape& S : Shapes)
		{
			N += S.Type == Type ? 1 : 0;
		}
		return N;
	}

	/** Compare vertical probes at the plan points Sample draws, between Region's top and bottom: Trace gives what Chaos
	 *  sees, the shapes what was exported. */
	void Probe(FAutomationTestBase& Test, const FString& Name, const FBox& Region, TFunctionRef<FVector2D(FRandomStream&)> Sample,
		const TArray<FRammsCollisionShape>& Shapes, TFunctionRef<TOptional<double>(const FVector&, const FVector&)> Trace, double Tolerance,
		double MissFraction, FRandomStream& Rs)
	{
		constexpr int32 Probes = 300;
		int32 Misses = 0, Compared = 0;
		double Worst = 0.0;
		FVector WorstAt = FVector::ZeroVector;
		for (int32 p = 0; p < Probes; ++p)
		{
			const FVector2D At = Sample(Rs);
			const double X = At.X;
			const double Y = At.Y;
			const TOptional<double> ChaosTop = Trace(FVector(X, Y, Region.Max.Z + 50.0), FVector(X, Y, Region.Min.Z - 50.0));
			const TOptional<double> Exported = ShapesTop(Shapes, X, Y);
			if (ChaosTop.IsSet() != Exported.IsSet())
			{
				++Misses;
				continue;
			}
			if (!ChaosTop.IsSet())
			{
				continue;
			}
			++Compared;
			const double Error = FMath::Abs(ChaosTop.GetValue() - Exported.GetValue());
			if (Error > Worst)
			{
				Worst = Error;
				WorstAt = FVector(X, Y, ChaosTop.GetValue());
			}
		}
		Test.TestTrue(FString::Printf(TEXT("%s: probes find a top (%d of %d)"), *Name, Compared, Probes), Compared > Probes / 10);
		Test.TestTrue(FString::Printf(TEXT("%s: %d probes hit on one side only (allowed %d)"), *Name, Misses, FMath::CeilToInt(Probes * MissFraction)),
			Misses <= FMath::CeilToInt(Probes * MissFraction));
		Test.TestTrue(FString::Printf(TEXT("%s: probe tops within %.1f mm (worst %.2f mm at %s)"), *Name, Tolerance * 10.0, Worst * 10.0, *WorstAt.ToString()),
			Worst <= Tolerance);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsProcPieceCollisionShapesTest, "Ramms.WorldGen.CollisionShapes.ProcPieces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRammsProcPieceCollisionShapesTest::RunTest(const FString& Parameters)
{
	using namespace RammsCollisionShapesTest;
	FScratchWorld Scratch;
	if (!TestTrue(TEXT("world opens"), Scratch.Open()))
	{
		return false;
	}

	TArray<FCase> Cases;
	auto Add = [&Cases](const TCHAR* Name, ERammsProcPieceKind Kind, TFunction<void(ARammsProcPiece&)> Setup, const FRotator& Rotation = FRotator::ZeroRotator) -> FCase&
	{
		FCase& C = Cases.AddDefaulted_GetRef();
		C.Name = Name;
		C.Kind = Kind;
		C.Setup = MoveTemp(Setup);
		C.Rotation = Rotation;
		return C;
	};
	{
		FCase& C = Add(TEXT("Slab"), ERammsProcPieceKind::Slab, [](ARammsProcPiece& P) { P.SizeX = 300.0; P.SizeY = 200.0; P.SizeZ = 20.0; });
		C.Boxes = 1; C.Convex = 0; C.Grids = 0;
	}
	Add(TEXT("Slab pitched and rolled"), ERammsProcPieceKind::Slab, [](ARammsProcPiece& P) { P.SizeX = 400.0; P.SizeY = 250.0; P.SizeZ = 15.0; }, FRotator(4.0, 17.0, 3.0)).Boxes = 1;
	Add(TEXT("Curb"), ERammsProcPieceKind::Curb, [](ARammsProcPiece& P) { P.SizeX = 400.0; P.SizeY = 15.0; P.Rise = 15.0; }).Boxes = 1;
	{
		FCase& C = Add(TEXT("CurbRamp"), ERammsProcPieceKind::CurbRamp, [](ARammsProcPiece& P) { P.Rise = 15.0; P.Slope = 0.0833; P.SizeY = 120.0; P.FlareSlope = 0.1; P.WarningDepth = 61.0; }, FRotator(0.0, 30.0, 0.0));
		C.Boxes = 1; C.Convex = 1; C.Grids = 2;
	}
	{
		// A graded street tilts its pieces: the shapes must keep the whole transform.
		FCase& C = Add(TEXT("CurbRamp pitched and rolled"), ERammsProcPieceKind::CurbRamp, [](ARammsProcPiece& P) { P.Rise = 18.0; P.Slope = 0.07; P.SizeY = 150.0; P.FlareSlope = 0.08; P.WarningDepth = 0.0; }, FRotator(3.0, -25.0, -2.0));
		C.Boxes = 0; C.Convex = 1; C.Grids = 2;
	}
	Add(TEXT("Steps"), ERammsProcPieceKind::Step, [](ARammsProcPiece& P) { P.StepCount = 3; P.SizeX = 30.0; P.Rise = 17.0; P.SizeY = 120.0; }, FRotator(2.0, 10.0, 2.0)).Boxes = 3;
	Add(TEXT("Ramp"), ERammsProcPieceKind::Ramp, [](ARammsProcPiece& P) { P.Rise = 30.0; P.Slope = 0.0833; P.SizeY = 120.0; P.LandingSize = 152.4; }).Boxes = 4;
	Add(TEXT("Wall with a door and a window"), ERammsProcPieceKind::Wall, [](ARammsProcPiece& P)
	{
		P.SizeX = 500.0; P.SizeY = 20.0; P.SizeZ = 300.0; P.OpeningWidth = 100.0; P.OpeningOffset = 120.0; P.Openings = TEXT("340,120,90,210");
	});
	Add(TEXT("Door"), ERammsProcPieceKind::Door, [](ARammsProcPiece& P) { P.SizeX = 90.0; P.SizeY = 4.0; P.SizeZ = 210.0; P.Rise = 2.0; }).Boxes = 6;
	Add(TEXT("Bollard"), ERammsProcPieceKind::Bollard, [](ARammsProcPiece& P) { P.SizeX = 20.0; P.SizeZ = 90.0; }).Convex = 1;
	// Canopy blobs are sphere-boxes folded outward (FoldOutward), so each blob's mesh is its hull: exact like the rest.
	Add(TEXT("Tree"), ERammsProcPieceKind::Tree, [](ARammsProcPiece& P) { P.SizeX = 300.0; P.SizeZ = 600.0; });
	Add(TEXT("Shrub"), ERammsProcPieceKind::Shrub, [](ARammsProcPiece& P) { P.SizeX = 80.0; P.SizeZ = 60.0; }).Convex = 4;
	{
		FCase& C = Add(TEXT("Roof"), ERammsProcPieceKind::Roof, [](ARammsProcPiece& P) { P.SizeX = 600.0; P.SizeY = 800.0; P.SizeZ = 150.0; }, FRotator(0.0, 40.0, 0.0));
		C.Boxes = 1; C.Convex = 1;
	}
	Add(TEXT("Planter"), ERammsProcPieceKind::Planter, [](ARammsProcPiece& P) { P.SizeX = 100.0; P.SizeY = 60.0; P.SizeZ = 50.0; }).Boxes = 2;
	Add(TEXT("Column"), ERammsProcPieceKind::Column, [](ARammsProcPiece& P) { P.SizeX = 40.0; P.SizeZ = 300.0; }).Boxes = 1;
	{
		FCase& C = Add(TEXT("Mound"), ERammsProcPieceKind::Mound, [](ARammsProcPiece& P) { P.SizeX = 400.0; P.SizeY = 300.0; P.Rise = 40.0; }, FRotator(0.0, 15.0, 0.0));
		C.bClosedMesh = false; C.Grids = 1;
	}
	{
		FCase& C = Add(TEXT("Hollow"), ERammsProcPieceKind::Mound, [](ARammsProcPiece& P) { P.SizeX = 300.0; P.SizeY = 300.0; P.Rise = -25.0; }, FRotator(2.0, 0.0, -1.5));
		C.bClosedMesh = false; C.Grids = 1;
	}
	{
		FCase& C = Add(TEXT("Patch"), ERammsProcPieceKind::Patch, [](ARammsProcPiece& P) { P.SizeX = 200.0; P.SizeY = 120.0; P.SizeZ = 1.6; });
		C.bClosedMesh = false; C.Convex = 1;
	}

	FRandomStream Rs(20261114);
	for (int32 c = 0; c < Cases.Num(); ++c)
	{
		const FCase& Case = Cases[c];
		ARammsProcPiece* Piece = Scratch.Spawn<ARammsProcPiece>(FTransform(Case.Rotation, FVector(2500.0 * c, 700.0, 120.0)));
		if (!TestNotNull(*FString::Printf(TEXT("%s: spawns"), *Case.Name), Piece))
		{
			continue;
		}
		Piece->Kind = Case.Kind;
		Piece->KindId = -1;
		Case.Setup(*Piece);
		Piece->Rebuild();

		TArray<FRammsCollisionShape> Shapes;
		Piece->AppendCollisionShapes(Shapes);
		if (!TestTrue(*FString::Printf(TEXT("%s: has shapes"), *Case.Name), Shapes.Num() > 0))
		{
			continue;
		}
		for (const FRammsCollisionShape& S : Shapes)
		{
			TestFalse(*FString::Printf(TEXT("%s: every shape has a surface"), *Case.Name), S.Surface.IsNone());
			TestTrue(*FString::Printf(TEXT("%s: shape frames carry no scale"), *Case.Name), S.Transform.GetScale3D().Equals(FVector::OneVector, 1.0e-6));
			if (S.Type == ERammsCollisionShapeType::Convex)
			{
				TestTrue(*FString::Printf(TEXT("%s: hulls have volume"), *Case.Name), S.Points.Num() >= 4);
			}
			if (S.Type == ERammsCollisionShapeType::HeightGrid)
			{
				TestTrue(*FString::Printf(TEXT("%s: grids are whole"), *Case.Name), S.NumX >= 2 && S.NumY >= 2 && S.Heights.Num() == S.NumX * S.NumY && S.BaseDepth > 0.0f);
			}
		}
		if (Case.Boxes >= 0)
		{
			TestEqual(*FString::Printf(TEXT("%s: boxes"), *Case.Name), CountOf(Shapes, ERammsCollisionShapeType::Box), Case.Boxes);
		}
		if (Case.Convex >= 0)
		{
			TestEqual(*FString::Printf(TEXT("%s: hulls"), *Case.Name), CountOf(Shapes, ERammsCollisionShapeType::Convex), Case.Convex);
		}
		if (Case.Grids >= 0)
		{
			TestEqual(*FString::Printf(TEXT("%s: height grids"), *Case.Name), CountOf(Shapes, ERammsCollisionShapeType::HeightGrid), Case.Grids);
		}

		// Bounds: a millimetre either way. A closed mesh is a solid like the shapes; a sheet (a mound, a patch's disc) is
		// compared with the shapes' upper surfaces, its solid only checked to reach below it.
		const FBox Mesh = MeshBounds(*Piece);
		const FBox Solid = ShapeBounds(Shapes);
		const FBox Exported = Case.bClosedMesh ? Solid : SurfaceBounds(Shapes);
		constexpr double Mm = 0.1;
		const bool bPlan = FMath::Abs(Mesh.Min.X - Exported.Min.X) <= Mm && FMath::Abs(Mesh.Max.X - Exported.Max.X) <= Mm
			&& FMath::Abs(Mesh.Min.Y - Exported.Min.Y) <= Mm && FMath::Abs(Mesh.Max.Y - Exported.Max.Y) <= Mm;
		TestTrue(*FString::Printf(TEXT("%s: plan bounds within 1 mm (mesh %s, shapes %s)"), *Case.Name, *Mesh.ToString(), *Exported.ToString()), bPlan);
		TestTrue(*FString::Printf(TEXT("%s: top within 1 mm (mesh %.2f, shapes %.2f)"), *Case.Name, Mesh.Max.Z, Exported.Max.Z), FMath::Abs(Mesh.Max.Z - Exported.Max.Z) <= Mm);
		if (Case.bClosedMesh)
		{
			TestTrue(*FString::Printf(TEXT("%s: bottom within 1 mm (mesh %.2f, shapes %.2f)"), *Case.Name, Mesh.Min.Z, Exported.Min.Z), FMath::Abs(Mesh.Min.Z - Exported.Min.Z) <= Mm);
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s: the solid reaches below the sheet"), *Case.Name), Solid.Min.Z <= Mesh.Min.Z + Mm);
		}

		UDynamicMeshComponent* Component = Piece->MeshComponent;
		const FBox Local = LocalMeshBounds(*Piece);
		const FTransform PieceXf = Component->GetComponentTransform();
		auto InPiece = [&Local, &PieceXf](FRandomStream& R)
		{
			const FVector W = PieceXf.TransformPosition(FVector(FMath::Lerp(Local.Min.X, Local.Max.X, R.FRand()), FMath::Lerp(Local.Min.Y, Local.Max.Y, R.FRand()), Local.Max.Z));
			return FVector2D(W.X, W.Y);
		};
		Probe(*this, Case.Name, Mesh, InPiece, Shapes, [Component](const FVector& From, const FVector& To) -> TOptional<double>
		{
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(RammsShapeProbe), /*bTraceComplex=*/true);
			if (Component->LineTraceComponent(Hit, From, To, Params))
			{
				return Hit.ImpactPoint.Z;
			}
			return TOptional<double>();
		}, Case.Tolerance, Case.MissFraction, Rs);
	}

	// A decal has no collision at all, and adds nothing.
	if (ARammsProcPiece* Decal = Scratch.Spawn<ARammsProcPiece>(FTransform(FVector(-3000.0, 0.0, 0.0))))
	{
		Decal->Kind = ERammsProcPieceKind::Decal;
		Decal->KindId = -1;
		Decal->Rebuild();
		TArray<FRammsCollisionShape> Shapes;
		Decal->AppendCollisionShapes(Shapes);
		TestEqual(TEXT("Decal: no shapes"), Shapes.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRammsInstancedCollisionShapesTest, "Ramms.WorldGen.CollisionShapes.InstancedMeshes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRammsInstancedCollisionShapesTest::RunTest(const FString& Parameters)
{
	using namespace RammsCollisionShapesTest;
	FScratchWorld Scratch;
	if (!TestTrue(TEXT("world opens"), Scratch.Open()))
	{
		return false;
	}
	ARammsWorldGenerator* Generator = Scratch.Spawn<ARammsWorldGenerator>(FTransform::Identity);
	if (!TestNotNull(TEXT("generator spawns"), Generator))
	{
		return false;
	}

	// The engine's basic shapes carry a box, a sphere and a convex hull for simple collision. Each is placed plainly,
	// rotated, and scaled unevenly: Chaos keeps a box's axes and scales its extents, and gives a sphere its smallest
	// scale, and the shapes must follow the same rules.
	const TCHAR* const Paths[] = { TEXT("/Engine/BasicShapes/Cube.Cube"), TEXT("/Engine/BasicShapes/Sphere.Sphere"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder") };
	const FTransform Placements[] = {
		FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(1.0)),
		FTransform(FRotator(10.0, 30.0, 5.0), FVector::ZeroVector, FVector(1.0)),
		FTransform(FRotator(0.0, 20.0, 0.0), FVector::ZeroVector, FVector(2.0, 0.5, 0.3)),
		FTransform(FRotator(8.0, -15.0, 6.0), FVector::ZeroVector, FVector(1.5, 1.2, 0.8)),
	};
	struct FPlaced
	{
		FString Name;
		FBox Region;
		UInstancedStaticMeshComponent* Instances = nullptr;
		int32 Index = INDEX_NONE;
	};
	TArray<FPlaced> Placed;
	for (int32 m = 0; m < static_cast<int32>(UE_ARRAY_COUNT(Paths)); ++m)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, Paths[m]);
		if (!TestNotNull(*FString::Printf(TEXT("%s loads"), Paths[m]), Mesh))
		{
			continue;
		}
		UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(Generator);
		Instances->SetStaticMesh(Mesh);
		Instances->SetMobility(EComponentMobility::Movable);
		Instances->SetCollisionProfileName(TEXT("BlockAll"));
		Instances->SetupAttachment(Generator->GetRootComponent());
		Instances->RegisterComponent();
		for (int32 p = 0; p < static_cast<int32>(UE_ARRAY_COUNT(Placements)); ++p)
		{
			FTransform Xf = Placements[p];
			Xf.SetLocation(FVector(800.0 * p, 1000.0 * (m + 1), 200.0));
			const int32 Index = Instances->AddInstance(Xf, /*bWorldSpace=*/true);
			Placed.Add({ FString::Printf(TEXT("%s placement %d"), *FPaths::GetBaseFilename(Paths[m]), p), Mesh->GetBoundingBox().TransformBy(Xf), Instances, Index });
		}
	}

	TArray<FRammsCollisionShape> Shapes;
	Generator->GatherCollisionShapes(Shapes);
	TestTrue(TEXT("instances become shapes"), Shapes.Num() >= Placed.Num());
	AddInfo(FString::Printf(TEXT("%d shapes: %d boxes, %d hulls, %d spheres, %d capsules"), Shapes.Num(), CountOf(Shapes, ERammsCollisionShapeType::Box),
		CountOf(Shapes, ERammsCollisionShapeType::Convex), CountOf(Shapes, ERammsCollisionShapeType::Sphere), CountOf(Shapes, ERammsCollisionShapeType::Capsule)));

	FRandomStream Rs(4242);
	UWorld* World = Scratch.World;
	for (const FPlaced& P : Placed)
	{
		// Probe where Chaos's own body is (a sphere scaled (2, 0.5, 0.3) is a 15 cm ball in a 2 m render box), a centimetre
		// beyond it each way so a shape that reached past it would show.
		// (UE 5.8 keeps instance bodies behind GetBodyInstance(..., Index); the old InstanceBodies array stays empty.)
		FBox Region = P.Region;
		const FBodyInstance* Body = P.Instances ? P.Instances->GetBodyInstance(NAME_None, /*bGetWelded=*/true, P.Index) : nullptr;
		const FBox BodyBounds = Body ? Body->GetBodyBounds() : FBox(ForceInit);
		if (BodyBounds.IsValid && BodyBounds.GetSize().GetMin() > 0.0)
		{
			Region = BodyBounds.ExpandBy(1.0);
		}
		else
		{
			AddWarning(FString::Printf(TEXT("%s: no Chaos body bounds for the instance; probing its render bounds"), *P.Name));
		}
		auto InBody = [&Region](FRandomStream& R) { return FVector2D(FMath::Lerp(Region.Min.X, Region.Max.X, R.FRand()), FMath::Lerp(Region.Min.Y, Region.Max.Y, R.FRand())); };
		Probe(*this, P.Name, Region, InBody, Shapes, [World](const FVector& From, const FVector& To) -> TOptional<double>
		{
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(RammsShapeProbe), /*bTraceComplex=*/false);
			if (World->LineTraceSingleByChannel(Hit, From, To, ECC_WorldStatic, Params))
			{
				return Hit.ImpactPoint.Z;
			}
			return TOptional<double>();
		}, 0.2, 0.02, Rs);
	}

	// A region keeps only what is near it.
	FRammsCollisionShapeFilter Near;
	Near.Centre = FVector(0.0, 1000.0, 0.0);
	Near.Radius = 300.0;
	TArray<FRammsCollisionShape> Filtered;
	Generator->GatherCollisionShapes(Filtered, Near);
	TestTrue(TEXT("a region keeps fewer shapes"), Filtered.Num() > 0 && Filtered.Num() < Shapes.Num());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
