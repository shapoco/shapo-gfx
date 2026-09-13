// Static scene traversal for Graphics3D (Mesh / Node / Scene).

#include "shapoco/gfx3d/gfx3d.hpp"

namespace shapoco::gfx3d {

void Graphics3D::putMesh(const Mesh &mesh) {
  if (!mesh.primitives) return;
  for (uint16_t i = 0; i < mesh.primitiveCount; i++)
    putPrimitive(mesh.primitives[i]);
}

void Graphics3D::putNode(const Node &node, NodeVisitor *visitor) {
  mat4f local = node.transform;
  if (visitor && !visitor->onNode(node, local)) return;
  if (!pushState()) {  // stack full: skip this subtree
    nodesDropped_++;
    return;
  }
  transform(local);
  if (node.mesh) putMesh(*node.mesh);
  if (node.children) {
    for (uint16_t i = 0; i < node.childCount; i++) {
      if (node.children[i]) putNode(*node.children[i], visitor);
    }
  }
  popState();
}

void Graphics3D::putScene(const Scene &scene, NodeVisitor *visitor) {
  if (!scene.roots) return;
  for (uint16_t i = 0; i < scene.rootCount; i++) {
    if (scene.roots[i]) putNode(*scene.roots[i], visitor);
  }
}

}  // namespace shapoco::gfx3d
