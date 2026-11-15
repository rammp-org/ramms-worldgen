// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "PCG/RammsPCGCommon.h"

struct FRammsPaletteMesh;

/**
 * The ground of a street world: one deterministic elevation function every street node evaluates, so roads, curbs,
 * sidewalks, lots, parks and everything placed on them agree to the millimetre. The Street Network builds it (from
 * the seed, the street grid and the terrain parameters) and hands it downstream as a text attribute (RammsGround::Attr
 * on its Blocks and Sidewalks points; Subdivide Lots and Park Layout pass it on), and every node parses the same text,
 * so every node computes the same heights.
 *
 * The surfaces, all piecewise planar so they are exact as boxes and convex prisms:
 *  - Base(x, y) = Z0 + FX(x) + FY(y): FX / FY are piecewise linear over the street lines (linear beyond the outer
 *    ones), so every street climbs at its own constant grade between intersections and every grid cell is one plane.
 *  - Roads: Base - CurbHeight at the gutters, Crown * (RoadWidth / 2 - distance from the centre line) higher toward
 *    the centre line. In an intersection square each of its four triangles (towards an edge) carries the crown of the
 *    road through that edge, or of the other road where that edge is a curb: continuous, eight planar facets.
 *  - Blocks: Base - CurbHeight + Reveal (the block's curb height, a plane: linear along each curb) at the curb line,
 *    rising inward at the block's CrossSlope up to the sidewalk's back edge (the distance to the nearest curb line,
 *    capped at SidewalkWidth), level with that edge over the lots. A corner square splits on its diagonal.
 *
 * With every terrain parameter at 0 the ground is the original flat world: sidewalks at Z0, roads CurbHeight below.
 */
struct RAMMSWORLDGEN_API FRammsGroundModel
{
	struct FBlock
	{
		/** The block's street centre lines (its outer rectangle); the curb lines are RoadWidth / 2 inside them. */
		double X0 = 0.0, X1 = 0.0, Y0 = 0.0, Y1 = 0.0;
		/** Curb height above the gutter at the block's centre (cm), and its gradient (cm per cm). */
		double Reveal = 15.0, RevealX = 0.0, RevealY = 0.0;
		/** Sidewalk cross slope: how fast the walk rises from the curb toward the lots (rise / run). */
		double CrossSlope = 0.0;
	};

	/** Live road arms at a line node: E = +X, N = +Y, W = -X, S = -Y. */
	enum EArm : uint8 { ArmE = 1, ArmN = 2, ArmW = 4, ArmS = 8 };

	double Z0 = 0.0;
	double RoadWidth = 700.0;
	double SidewalkWidth = 300.0;
	/** Nominal curb height: Base - CurbHeight is the gutter. */
	double CurbHeight = 15.0;
	double Crown = 0.0;
	/** Street centre lines, and the height offsets of Base along them. */
	TArray<double> X, Y, FX, FY;
	/** Per line node (j * X.Num() + i): EArm bits. */
	TArray<uint8> Arms;
	TArray<FBlock> Blocks;

	bool IsValid() const { return X.Num() >= 2 && Y.Num() >= 2 && FX.Num() == X.Num() && FY.Num() == Y.Num(); }

	/** Exact text form (17 significant digits) and back. */
	FString ToString() const;
	bool FromString(const FString& Text);

	/** The reference height Z0 + FX(x) + FY(y): sidewalk level for a CurbHeight curb on a level walk. */
	double Base(double InX, double InY) const;
	/** Its slopes along X and Y at a point (one cell's plane; on a street line, the cell to the +X / +Y side). */
	FVector2D BaseSlope(double InX, double InY) const;
	/** The block whose curb rectangle (grown by Margin) holds the point, or INDEX_NONE (a road, or off the site). */
	int32 FindBlock(double InX, double InY, double Margin = 0.0) const;
	/** The block's curb rectangle (the curb lines). */
	FBox2D CurbRect(int32 Block) const;
	/** The street centre lines inside the block's curb rectangle (dropped streets): the ground bends along them. */
	void InnerLines(int32 Block, TArray<double>& OutX, TArray<double>& OutY) const;
	/** Curb height above the gutter at a point of the block (the plane, evaluated anywhere). */
	double Reveal(int32 Block, double InX, double InY) const;
	/** Road surface height (roads, intersections; off every road: the gutter level). */
	double RoadZ(double InX, double InY) const;
	/** Walking surface of a block (sidewalk ring, then the level ground over the lots). */
	double BlockZ(int32 Block, double InX, double InY) const;
	/** Whatever ground is there: a block's surface inside its curb lines, else the road. */
	double GroundZ(double InX, double InY) const;
	/** The ground's normal at a point (central differences over 1 cm: a crease averages its two faces). */
	FVector Normal(double InX, double InY) const;
	/** The sidewalk's cross slope at the block (a shortcut for the manifest). */
	double CrossSlope(int32 Block) const { return Blocks.IsValidIndex(Block) ? Blocks[Block].CrossSlope : 0.0; }
};

/** Parses the ground attribute once per distinct text (all points of a generation carry the same one). */
struct RAMMSWORLDGEN_API FRammsGroundCache
{
	/** The model for this text, or null when it is empty or does not parse (the flat world: no model). */
	const FRammsGroundModel* Get(const FString& Text);

private:
	FString Key;
	FRammsGroundModel Model;
	bool bValid = false;
};

namespace RammsGround
{
	/** The serialized FRammsGroundModel on Blocks / Lots / Parks / Sidewalks points. Absent: the flat world. */
	inline const FName Attr(TEXT("Ground"));
	/** Subtype of ground slabs that are no manifest feature: keeps a slab on high ground from being labelled a roof. */
	inline const FName GroundSubtype(TEXT("Ground"));
	/** Optional palette slot for brick pavers (Mat.Facade's brick stands in without one). */
	inline const FName MatBrick(TEXT("Mat.Brick"));
	/** A route's polyline on graded ground, "x,y,z;..." (Hazard Placer -> Manifest). */
	inline const FName RoutePoints(TEXT("RoutePoints"));

	// --- planar polygons -----------------------------------------------------------------------------------
	/** The signed area of a polygon in XY, positive when counter-clockwise. */
	RAMMSWORLDGEN_API double SignedArea(const TArray<FVector2D>& Poly);
	/** The counter-clockwise outline of a box. */
	RAMMSWORLDGEN_API TArray<FVector2D> BoxPoly(const FBox2D& Box);
	/** The part of a convex polygon where N . p <= C (empty when none is left). */
	RAMMSWORLDGEN_API void ClipHalfPlane(const TArray<FVector2D>& In, const FVector2D& N, double C, TArray<FVector2D>& Out);
	/** A convex polygon split by the line N . p = C into its parts below (N . p <= C) and above it. */
	RAMMSWORLDGEN_API void SplitByLine(const TArray<FVector2D>& In, const FVector2D& N, double C, TArray<FVector2D>& Below, TArray<FVector2D>& Above);
	/** Splits every polygon of Polys by the line where it crosses it. */
	RAMMSWORLDGEN_API void SplitAllByLine(TArray<TArray<FVector2D>>& Polys, const FVector2D& N, double C);
	/** R minus Cut: R when they do not overlap, else up to four boxes around the cut. */
	RAMMSWORLDGEN_API void SubtractBox(const FBox2D& R, const FBox2D& Cut, TArray<FBox2D>& Out);

	// --- placing on a slope --------------------------------------------------------------------------------
	/** Rotation whose +X is the horizontal direction YawDeg laid onto the plane with normal N, and whose +Z is N. */
	RAMMSWORLDGEN_API FQuat OnPlane(double YawDeg, const FVector& N);
	/** Rotation whose +X is XDir exactly and whose +Z is N, made orthogonal to it (a curb along its line, a ramp). */
	RAMMSWORLDGEN_API FQuat FromAxes(const FVector& XDir, const FVector& N);
	/** A scaled cube instance as RammsPCG::WriteCube, lying on the plane with normal N (markings, crosswalk bars, stall lines). */
	RAMMSWORLDGEN_API int32 WriteCubeOn(FRammsPointWriter& W, const FVector& Centre, const FVector& Size, double YawDeg, const FVector& N, const FString& Material, int32 Seed);
	/** A palette mesh as RammsPCG::WritePickedMesh, tilted onto the plane with normal N (litter, grates, parked cars). */
	RAMMSWORLDGEN_API int32 WritePickedMeshOn(FRammsPointWriter& W, const FRammsPaletteMesh& Picked, const FVector& Location, double YawDeg, const FVector& N, int32 Seed, double ScaleMul = 1.0);

	// --- GroundPrisms pieces --------------------------------------------------------------------------------
	/**
	 * ARammsProcPiece::Parts: prisms separated by ';', each "<material slot>,<surface>:x,y,z x,y,z ..." -- the top
	 * polygon's vertices in the actor's frame (cm), convex and planar, counter-clockwise from above. Each prism is solid
	 * from its top down to SizeZ below its lowest top vertex.
	 */
	struct FPart
	{
		int32 Mat = 0;
		FName Surface;
		TArray<FVector> Top;
	};
	RAMMSWORLDGEN_API bool ParseParts(const FString& Text, TArray<FPart>& Out);

	/** Collects convex prisms (world top polygons) for one GroundPrisms piece placed at Origin (no rotation). */
	struct RAMMSWORLDGEN_API FPrisms
	{
		FVector Origin = FVector::ZeroVector;
		FString Text;
		int32 Num = 0;
		FBox2D Extent = FBox2D(ForceInit);

		explicit FPrisms(const FVector& InOrigin) : Origin(InOrigin) {}
		/** A prism from its top polygon in world space (convex and planar; either winding). */
		void Add(const TArray<FVector>& Top, int32 Mat, FName Surface);
		/** A prism over a convex world-XY polygon, its top at Height(p) at each vertex (planar inside one face of the ground). */
		void AddPoly(const TArray<FVector2D>& Poly, TFunctionRef<double(const FVector2D&)> Height, int32 Mat, FName Surface);
		bool IsEmpty() const { return Num == 0; }
		/** The piece: kind, transform, parts, SizeX / SizeY the plan extent (for the manifest), SizeZ the depth. */
		FRammsPieceDesc Desc(double Depth) const;
	};
}
