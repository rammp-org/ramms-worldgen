// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace UE::Geometry { class FDynamicMesh3; }

/**
 * Backend-neutral collision of generated geometry: what Chaos collides with, as primitive shapes in world
 * space, for physics engines that build their own world from it (MuJoCo through RammsMujocoSupport).
 *
 * Exactness is the point: a 6 mm panel lip, a curb face, a pothole's rim must stay a step of that height with a
 * vertical face, so pieces describe themselves as boxes, convex hulls and height grids rather than as a sampled
 * terrain. A shape's solid is closed: a box or hull is solid inside; a height grid is solid from its samples down
 * to BaseDepth below the lowest one.
 *
 * Producers: ARammsProcPiece::AppendCollisionShapes (per kind) and ARammsWorldGenerator::GatherCollisionShapes
 * (every piece plus the simple collision of the generator's instanced meshes).
 */
enum class ERammsCollisionShapeType : uint8
{
	/** Transform is the box centre and orientation; HalfExtents its half sizes (cm). */
	Box,
	/** Points (cm, in Transform's frame) span a convex solid. */
	Convex,
	/** A regular grid of heights in Transform's frame: sample (i, j) at (i * Spacing.X, j * Spacing.Y, Heights[j * NumX + i]),
	 *  i along X (NumX samples), j along Y (NumY samples); the frame's origin is sample (0, 0) at height 0. */
	//  Between samples the surface is two triangles per cell: the cell from (i, j) to (i + 1, j + 1) is split along the
	//  diagonal from (i + 1, j) to (i, j + 1). That is the split MuJoCo's height fields make once Unreal's Y is mirrored
	//  into MuJoCo's frame, so a mesh drawn from the grid must split its cells the same way
	//  (RammsCollisionShapes::MatchHeightGridDiagonals) for the two to agree inside a cell, not only at the samples.
	//  NumX and NumY are at least 2.
	HeightGrid,
	/** Transform is the centre; HalfExtents.X the radius (cm). Simple collision of instanced meshes. */
	Sphere,
	/** Transform is the centre, the axis along its local Z; HalfExtents.X the radius and HalfExtents.Z half the length of
	 *  the straight part (cm), so the rounded ends reach HalfExtents.Z + radius from the centre. Simple collision of
	 *  instanced meshes. */
	Capsule,
};

struct FRammsCollisionShape
{
	ERammsCollisionShapeType Type = ERammsCollisionShapeType::Box;

	/** World frame of the shape (scale 1: sizes are baked into the extents, points and spacing). */
	FTransform Transform = FTransform::Identity;

	/** Box: half sizes (cm). */
	FVector HalfExtents = FVector::ZeroVector;

	/** Convex: the hull's points (cm, in Transform's frame). */
	TArray<FVector3f> Points;

	/** HeightGrid: samples along X and Y, their spacing (cm), the heights (cm, row-major with X fastest) and how far
	 *  the solid extends below the lowest sample (cm). */
	int32 NumX = 0;
	int32 NumY = 0;
	FVector2D Spacing = FVector2D::ZeroVector;
	TArray<float> Heights;
	float BaseDepth = 20.0f;

	/** What the surface is, for friction and contact materials: Asphalt, Concrete, Curb, Brick, Cobble, Grass, Dirt,
	 *  Metal, Wood, Default... */
	FName Surface = NAME_None;

	/** Where it came from (actor or component name, piece kind), for debugging and reports. */
	FName Source = NAME_None;
};

/** Which part of a generated world GatherCollisionShapes describes. The default describes all of it. */
struct RAMMSWORLDGEN_API FRammsCollisionShapeFilter
{
	/** When valid: only shapes whose bounds intersect this box (world, cm). */
	FBox Region = FBox(ForceInit);

	/** When Radius > 0: only shapes whose bounds come within Radius of Centre in plan (XY, cm). */
	FVector Centre = FVector::ZeroVector;
	double Radius = 0.0;

	bool IsEverything() const { return !Region.IsValid && Radius <= 0.0; }

	/** Whether a shape (or a whole source) with these world bounds is kept. */
	bool Accepts(const FBox& WorldBounds) const;
};

/** Building, measuring and describing shapes: shared by the producers, their tests and the debug views. */
namespace RammsCollisionShapes
{
	/** A box in world space: Frame is its centre and orientation (any scale on it is ignored). */
	RAMMSWORLDGEN_API FRammsCollisionShape& AddBox(TArray<FRammsCollisionShape>& Out, const FTransform& Frame, const FVector& HalfExtents, FName Surface, FName Source);

	/** A convex hull of Points (cm, in Frame; any scale on Frame is ignored). */
	RAMMSWORLDGEN_API FRammsCollisionShape& AddConvex(TArray<FRammsCollisionShape>& Out, const FTransform& Frame, TArray<FVector3f>&& Points, FName Surface, FName Source);

	/** World-space bounds of a shape. */
	RAMMSWORLDGEN_API FBox GetBounds(const FRammsCollisionShape& Shape);

	/** The shape's closed surface as world-space triangles (three corners each). Convex shapes are hulled; spheres and
	 *  capsules are tessellated coarsely (for drawing and probing, not for physics). */
	RAMMSWORLDGEN_API void GetTriangles(const FRammsCollisionShape& Shape, TArray<FVector>& OutCorners);

	/** The highest point at which a vertical line through world (X, Y) meets the shape, if it does. */
	RAMMSWORLDGEN_API TOptional<double> TopAt(const FRammsCollisionShape& Shape, double X, double Y);

	/**
	 * Make a grid mesh split its cells the HeightGrid way, and read its samples.
	 *
	 * Mesh vertices from FirstVertex on that sit on the grid are matched to samples (GridFrame maps grid space, as in
	 * HeightGrid, into mesh space), the highest at each sample when a solid's bottom lies under it; each cell whose two
	 * triangles share the other diagonal has that edge flipped (attributes follow). OutHeights gets the samples'
	 * heights in grid space. Returns the number of edges flipped, or -1 when some sample has no vertex (the mesh is not
	 * that grid).
	 */
	RAMMSWORLDGEN_API int32 MatchHeightGridDiagonals(UE::Geometry::FDynamicMesh3& Mesh, int32 FirstVertex, const FTransform& GridFrame, int32 NumX, int32 NumY, const FVector2D& Spacing, TArray<float>& OutHeights);

	/** The surface a segmentation class stands on (URammsSegmentationLabels: Road, Sidewalk, Curb, Lawn, Hazard.Mud
	 *  ...): Asphalt, Concrete, Curb, Brick, Cobble, Grass, Dirt, Gravel, Mud, Water, Metal, Wood, Glass or Foliage;
	 *  NAME_None when the class says nothing about it. */
	RAMMSWORLDGEN_API FName SurfaceForClass(FName SegmentationClass);

	/** The same, guessed from a physical material's name (PM_Asphalt, PhysMat_Concrete ...); NAME_None when unknown. */
	RAMMSWORLDGEN_API FName SurfaceForPhysicalMaterial(const FString& PhysicalMaterialName);

	/** "Box", "Convex", "HeightGrid", "Sphere", "Capsule". */
	RAMMSWORLDGEN_API const TCHAR* TypeName(ERammsCollisionShapeType Type);

	/** One past the last ERammsCollisionShapeType, for tables indexed by type. */
	constexpr int32 NumTypes = static_cast<int32>(ERammsCollisionShapeType::Capsule) + 1;
}
