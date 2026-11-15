// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace UE::Geometry { class FDynamicMesh3; }
class AActor;

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

	/** Actors whose collision another engine already has (MuJoCo quick-convert components): skipped whole. */
	TSet<const AActor*> ExcludedActors;

	/** Merge flush neighbouring boxes into one (RammsCollisionShapes::MergeFlushBoxes): paving and road cubes that tile
	 *  a flat area, slabs side by side. Off keeps one shape per source part. */
	bool bMergeFlushBoxes = true;

	/** Whether the region keeps everything (ExcludedActors are skipped either way). */
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

	/**
	 * The solid under one triangle of a surface: the triangle (A, B, C) and its drop straight down to BottomZ, in a
	 * local space that Scale (any sign) and then Frame (its rotation and location) take to the world. A triangulated
	 * surface described as one prism per triangle is exact whatever its shape and whatever the scale: the fallback
	 * where a height grid or a single hull cannot describe it. A triangle seen edge-on from above adds nothing.
	 */
	RAMMSWORLDGEN_API void AddTrianglePrism(TArray<FRammsCollisionShape>& Out, const FTransform& Frame, const FVector& Scale, const FVector& A, const FVector& B, const FVector& C, double BottomZ, FName Surface, FName Source);

	/**
	 * Merge flush neighbouring boxes, from FirstIndex on, wherever two of them make one box exactly: the same surface and
	 * orientation, the same extent on two axes, touching along the third (all within Tolerance, cm). Two geoms meeting
	 * at a seam give a body crossing it a contact on the far one's edge, a sideways push a flat surface does not have;
	 * one box has no seam. Boxes offset by a lip, or with a gap, stay apart. Order is kept otherwise. Returns how many
	 * boxes were merged away.
	 */
	RAMMSWORLDGEN_API int32 MergeFlushBoxes(TArray<FRammsCollisionShape>& Shapes, int32 FirstIndex = 0, double Tolerance = 0.01);

	/** AppendComponentHulls' default cap on hulls per mesh. */
	constexpr int32 DefaultMaxComponentHulls = 64;

	/**
	 * The convex hulls of a triangle mesh's connected parts: the closest convex description of a mesh Chaos collides
	 * with as its triangles. A table is its top and its legs, open between them, where one hull of the whole mesh is a
	 * solid block. A part whose bounds lie inside a larger part's (a sill on a wall) joins that part; beyond MaxHulls
	 * the smallest parts join the part nearest them. A part that is itself concave is still solid inside its hull.
	 * Each hull keeps only its points on the hull (all of them when they span no volume; parts under 3 points are
	 * dropped). Returns the number of connected parts found.
	 */
	RAMMSWORLDGEN_API int32 AppendComponentHulls(const TArray<FVector3f>& Vertices, const TArray<FIntVector>& Triangles, TArray<TArray<FVector>>& OutHulls, int32 MaxHulls = DefaultMaxComponentHulls);

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
	 * heights in grid space. Returns the number of edges flipped, or -1 when the mesh is not that grid: some sample has
	 * no vertex, or some cell is not two triangles sharing one of its diagonals, or its edge would not flip. Every cell
	 * that can be matched still is, so a caller that falls back to the mesh's own triangles describes the mesh as built.
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

	/** One past the last ERammsCollisionShapeType, for tables indexed by type: move it when a type is appended. */
	constexpr int32 NumTypes = static_cast<int32>(ERammsCollisionShapeType::Capsule) + 1;
}
