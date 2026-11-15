// Copyright ATDev. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "PCG/RammsPCGCommon.h"

struct FRammsPaletteMesh;

/**
 * The ground of a street world: one deterministic elevation function every street node evaluates, so roads, curbs,
 * sidewalks, lots, parks and everything placed on them agree to the millimetre. The Street Network builds it (from
 * the seed, the street grid and the terrain parameters) and hands it downstream as a text attribute (RammsGround::Attr
 * on its Blocks and Sidewalks points; Subdivide Lots and Park Layout pass it on), and every node parses the same text
 * and builds the same facets from it (Build), so every node computes the same heights.
 *
 * The surfaces, all piecewise planar so they are exact as boxes and convex prisms:
 *  - Base(x, y) = Z0 + FX(x) + FY(y): FX / FY are piecewise linear over the street lines (linear beyond the outer
 *    ones), so every street climbs at its own constant grade between intersections. It sets the gutters and the lots.
 *  - Roads: Base - CurbHeight at the gutters, Crown * (RoadWidth / 2 - distance from the centre line) higher toward
 *    the centre line. In an intersection square each of its four triangles (towards an edge) carries the crown of the
 *    road through that edge, or of the other road where that edge is a curb: continuous, eight planar facets.
 *  - Walks: the curb's top (the gutter plus the block's curb height, Reveal: a plane, so linear along each curb) along
 *    the curb line, rising inward at exactly the block's CrossSlope to the sidewalk's back edge, whatever the hill does
 *    across the street: a walk's running slope is its street's, its cross slope its block's setting.
 *  - Corners: two walks of different running grades meet at a block corner, so their back edges cannot meet there at
 *    one height. The corner zone (the corner square and CornerArm beyond it along each side) is a fan of planar
 *    triangles from the curb corner: the triangle along each curb is still that walk's plane; between them the ground
 *    turns from one walk's plane to the other's (the warp), continuous everywhere.
 *  - Lots (the block inside the sidewalk ring): the hill, Base - CurbHeight + Reveal + CrossSlope * SidewalkWidth (one
 *    plane per street-grid cell, bending along streets dropped inside a merged block), inside a ring of planar
 *    triangles that meets every walk's back edge exactly: the lots absorb what the level walks leave of the hill.
 *
 * With every terrain parameter at 0 there is no model: the original flat world, sidewalks at Z0, roads CurbHeight below.
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

	/** A planar facet of a block's ground (Build): a convex polygon, counter-clockwise from above, and its plane. */
	struct FFace
	{
		TArray<FVector2D> Poly;
		FBox2D Bounds = FBox2D(ForceInit);
		/** z = Z + G.X * (x - O.X) + G.Y * (y - O.Y). */
		FVector2D O = FVector2D::ZeroVector;
		double Z = 0.0;
		FVector2D G = FVector2D::ZeroVector;
		/** 0 straight walk, 1 corner zone, 2 lot ring, 3 lot interior. */
		uint8 Zone = 0;

		double At(double InX, double InY) const { return Z + G.X * (InX - O.X) + G.Y * (InY - O.Y); }
	};

	/** Live road arms at a line node: E = +X, N = +Y, W = -X, S = -Y. */
	enum EArm : uint8 { ArmE = 1, ArmN = 2, ArmW = 4, ArmS = 8 };

	double Z0 = 0.0;
	double RoadWidth = 700.0;
	double SidewalkWidth = 300.0;
	/** Nominal curb height: Base - CurbHeight is the gutter. */
	double CurbHeight = 15.0;
	double Crown = 0.0;
	/** How far each corner zone reaches along a side beyond the corner square (cm); 0 = SidewalkWidth. */
	double CornerArm = 0.0;
	/** Street centre lines, and the height offsets of Base along them. */
	TArray<double> X, Y, FX, FY;
	/** Per line node (j * X.Num() + i): EArm bits. */
	TArray<uint8> Arms;
	TArray<FBlock> Blocks;
	/** Per block, the facets of its ground (derived: Build, run by FromString). */
	TArray<TArray<FFace>> Faces;

	bool IsValid() const { return X.Num() >= 2 && Y.Num() >= 2 && FX.Num() == X.Num() && FY.Num() == Y.Num(); }

	/** Exact text form (17 significant digits) and back; FromString builds the facets, and leaves the model as it was
	 *  when the text does not parse. */
	FString ToString() const;
	bool FromString(const FString& Text);
	/** Build every block's facets from the fields above. FromString does; call it after setting the fields by hand. */
	void Build();

	/** The reference height Z0 + FX(x) + FY(y). */
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
	/** The curb's top at a point of the block's curb line: the gutter plus the curb height there. */
	double CurbTop(int32 Block, double InX, double InY) const;
	/** Road surface height (roads, intersections; off every road: the gutter level). */
	double RoadZ(double InX, double InY) const;
	/** Walking surface of a block (its facets; a point just outside the curb rectangle takes the nearest facet's plane). */
	double BlockZ(int32 Block, double InX, double InY) const;
	/** Whatever ground is there: a block's surface inside its curb lines, else the road. */
	double GroundZ(double InX, double InY) const;
	/** The ground's normal at a point (central differences over 1 cm: a crease averages its two faces). */
	FVector Normal(double InX, double InY) const;
	/** The sidewalk's cross slope at the block. */
	double CrossSlope(int32 Block) const { return Blocks.IsValidIndex(Block) ? Blocks[Block].CrossSlope : 0.0; }
	/** Along a side of the block (0 south, 1 east, 2 north, 3 west, as the Street Network numbers them, measured from
	 *  the side's start corner), where its corner zones end: between T0 and T1 the walk is that side's own plane
	 *  (exactly its block's cross slope). False for an unknown block or side. */
	bool StraightRange(int32 Block, int32 Side, double& OutT0, double& OutT1) const;

	/** A convex polygon (world XY) of a block cut into the parts on each of its facets (convex, each on one plane), in
	 *  facet order; empty when it misses the block. */
	void ClipToFaces(int32 Block, const TArray<FVector2D>& Poly, TArray<TArray<FVector2D>>& Out) const;
	/** The lowest and highest walking surface of a block under a convex polygon (exact: over its parts' corners). */
	double MinOver(int32 Block, const TArray<FVector2D>& Poly) const;
	double MaxOver(int32 Block, const TArray<FVector2D>& Poly) const;

private:
	void BuildBlock(int32 Block, TArray<FFace>& Out) const;
	const FFace* FindFace(int32 Block, double InX, double InY) const;
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

/**
 * Sidewalk panels set high or low on rough walks (the Street Network's PanelDz attribute on Sidewalks and Blocks
 * points; Subdivide Lots passes it on to the lots): world XY rectangles and their offsets above the ground model (cm).
 * Whatever stands on a panel (a door's pad, a root heave, a driveway apron) stands on its top, not on the model.
 */
struct RAMMSWORLDGEN_API FRammsPanelOffsets
{
	struct FPanel
	{
		FBox2D R = FBox2D(ForceInit);
		double Dz = 0.0;
	};
	TArray<FPanel> Panels;

	/** "x0,y0,x1,y1,dz;..." (only panels off the model are listed). */
	void Parse(const FString& Text);
	FString ToString() const;
	/** The offset of the panel under a point (0 where none is listed). */
	double At(double InX, double InY) const;
	/** The lowest and highest offset under an axis-aligned world box (0 where it covers no listed panel). */
	void Range(const FBox2D& Box, double& OutMin, double& OutMax) const;
	/** An axis-aligned world box cut into its parts on each listed panel and the rest (offset 0). */
	void Split(const FBox2D& Box, TArray<TPair<FBox2D, double>>& Out) const;
};

namespace RammsGround
{
	/** The serialized FRammsGroundModel on Blocks / Lots / Parks / Sidewalks points. Absent: the flat world. */
	inline const FName Attr(TEXT("Ground"));
	/** Panel offsets (FRammsPanelOffsets) on the Sidewalks / Blocks / Lots points of a rough walk. */
	inline const FName PanelDzAttr(TEXT("PanelDz"));
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
	/** The part of convex polygon A inside convex polygon B (counter-clockwise), empty when they do not overlap. */
	RAMMSWORLDGEN_API void ClipConvex(const TArray<FVector2D>& A, const TArray<FVector2D>& B, TArray<FVector2D>& Out);
	/** R minus Cut: R when they do not overlap, else up to four boxes around the cut. */
	RAMMSWORLDGEN_API void SubtractBox(const FBox2D& R, const FBox2D& Cut, TArray<FBox2D>& Out);
	/** The plane through three values at the corners of a triangle, evaluated at P (barycentric interpolation). */
	RAMMSWORLDGEN_API double LerpTriangle(const FVector2D& A, const FVector2D& B, const FVector2D& C, double Va, double Vb, double Vc, const FVector2D& P);

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

	/** Collects convex prisms (world top polygons) for one GroundPrisms piece placed at Origin, turned by Yaw. */
	struct RAMMSWORLDGEN_API FPrisms
	{
		FVector Origin = FVector::ZeroVector;
		/** The piece's yaw (degrees), so its frame is the feature's (a curb ramp's +X points outward, as the CurbRamp
		 *  kind's does, and its manifest record reads like one). */
		double Yaw = 0.0;
		FString Text;
		int32 Num = 0;
		/** Plan extent in the piece's frame. */
		FBox2D Extent = FBox2D(ForceInit);

		explicit FPrisms(const FVector& InOrigin, double InYaw = 0.0) : Origin(InOrigin), Yaw(InYaw) {}
		/** A prism from its top polygon in world space (convex and planar; either winding). */
		void Add(const TArray<FVector>& Top, int32 Mat, FName Surface);
		/** A prism over a convex world-XY polygon, its top at Height(p) at each vertex (planar inside one face of the ground). */
		void AddPoly(const TArray<FVector2D>& Poly, TFunctionRef<double(const FVector2D&)> Height, int32 Mat, FName Surface);
		/** Prisms over a convex world-XY polygon of a block, one per facet it covers, their tops at the ground plus
		 *  Offset(p) (Offset must be planar over the polygon: a constant, a tilt, a ramp's dip). */
		void AddOnGround(const FRammsGroundModel& Ground, int32 Block, const TArray<FVector2D>& Poly, TFunctionRef<double(const FVector2D&)> Offset, int32 Mat, FName Surface);
		bool IsEmpty() const { return Num == 0; }
		/** The piece: kind, transform, parts, SizeX / SizeY the plan extent in its frame (for the manifest), SizeZ the depth. */
		FRammsPieceDesc Desc(double Depth) const;
	};
}
