#include <geode/mesh/quadric.h>

namespace geode {

Quadric compute_quadric(TriangleTopology const &mesh, const RawField<const Vector<real,3>, VertexId> X, VertexId v) {
  real total = 0;
  Quadric q;
  for (const auto e : mesh.outgoing(v)) {
    if (!mesh.is_boundary(e)) {
      total += q.add_face(mesh, X, mesh.face(e));
    }
  }

  // Normalize
  if (total) {
    q *= 1/total;
  }

  return q;
}

}
