// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

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
	HeightGrid,
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
