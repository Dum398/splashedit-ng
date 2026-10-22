// Bringing a mesh down to a triangle count (meshoptimizer's simplifier).
#pragma once

#include "scene.hh"

namespace splash {

struct SimplifyResult {
    int before = 0;           // triangles in
    int after = 0;            // triangles out
    float error = 0;          // deviation from the input, relative to its extent
    bool permissive = false;  // the second pass (normal seams may give) was used
    int partsRemoved = 0;     // connected parts of more than 4 triangles left with none
};

// Collapses edges of `mesh` until it has at most `target` triangles, or until
// no collapse is left that keeps its seams: UV and colour discontinuities,
// submesh (material) boundaries and open borders stay where they are, so
// `after` can end above `target`. When the first pass stops more than 10%
// above it, a second pass also lets collapses cross normal discontinuities
// (still not UV, colour or submesh ones) and the smaller result is kept.
// Each surviving vertex keeps its position, UV, colour and skin weights (a
// collapse removes vertices, it never blends them); its normal is one of the
// input's normals at that corner, so a caller wanting correct shading
// recomputes them. Submeshes keep their order; one can end up empty. Unused
// vertices are dropped. `target` <= 0 or at least the input's count leaves
// the mesh as it is. A connected part (by position) can collapse away whole;
// `partsRemoved` counts those that had more than 4 triangles.
SimplifyResult simplifyToTriangles(Mesh& mesh, int target);

}  // namespace splash
