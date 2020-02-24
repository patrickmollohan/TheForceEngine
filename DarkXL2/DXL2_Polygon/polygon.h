#pragma once
#include <DXL2_System/types.h>
#include <vector>
#define MAX_POLYGON_VTX 1024
#define MAX_CONVEX_POLYGONS 1024

// Handling complex polygons.
struct Polygon
{
	s32 vtxCount;
	Vec2f vtx[MAX_POLYGON_VTX];
};

struct Triangle
{
	Vec2f vtx[3];
};

// DXL2_Polygon uses the "Ear-clipping" algorithm to convert concave polygons into a set of convex polygons suitable for rendering.
// See https://www.geometrictools.com/Documentation/TriangulationByEarClipping.pdf for more information on the core algorithm.
// Note the geometrictools implementation was not used.
namespace DXL2_Polygon
{
	// Decompose a concave polygon with holes into convex polygons.
	// A contour is a complete polygon. If it is a hole than the winding should be reversed compared to the outer polygon.
	Triangle* decomposeComplexPolygon(u32 contourCount, const Polygon* contours, u32* outConvexPolyCount);

	bool init();
	void shutdown();
}
