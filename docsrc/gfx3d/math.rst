ベクトルと行列
################################################################################

ヘッダ: ``shapoco/gfx2d/math2d.hpp`` (2D)、``shapoco/gfx3d/math3d.hpp`` (3D)

2D 型 (``shapoco::gfx2d``)
================================================================================

.. csv-table::
   :header: "型", "説明"

   "``vec2f { float x, y; }``", "2D ベクトル。``+``, ``-``, ``*`` (スカラ)、``dot()``, ``lerp()``"
   "``vec2i { int x, y; }``", "整数座標。``+``, ``-``, ``*`` (整数)、``==``, ``!=``"
   "``colorf { float r, g, b, a; }``", "float 色 (3D API のマテリアルとライトで使用)。``+``, ``*`` (色・スカラ)、``lerp()``"
   "``Rect { int x, y, width, height; }``", "半開区間の矩形。``right()``, ``bottom()``, ``isEmpty()``, ``contains(x, y)``, ``normalized()``, ``intersect(r)``, ``offset(dx, dy)``"
   "``clamp01(float)``, ``clampInt(lo, hi, v)``", "クランプ"

3D 型 (``shapoco::gfx3d``)
================================================================================

``vec2f``, ``colorf``, ``clamp01``, ``lerp`` は ``gfx2d`` のものが再公開されています。

vec3f
--------------------------------------------------------------------------------

.. code-block:: cpp

   struct vec3f { float x, y, z; };

``+``, ``-``, 単項 ``-``, ``*`` (スカラ), ``dot()``, ``cross()``, ``length()``, ``normalize()``, ``lerp()``

mat4f
--------------------------------------------------------------------------------

.. code-block:: cpp

   struct mat4f {
     float m[16];   // 列優先: m[col * 4 + row] (OpenGL 互換)
   };

.. csv-table:: 生成関数 (static)
   :header: "関数", "説明"

   "``identity()``", "単位行列"
   "``translation(x, y, z)``", "平行移動"
   "``rotation(angle, axis)``", "軸回転 (ラジアン、軸は正規化不要)"
   "``scaling(x, y, z)``", "スケール"
   "``perspective(fovY, aspect, zNear, zFar)``", "透視投影"
   "``orthographic(l, r, b, t, zNear, zFar)``", "正射影"
   "``lookAt(eye, target, up)``", "視点行列"
   "``fromQuaternion(x, y, z, w)``", "単位四元数からの回転行列"

.. csv-table:: 演算
   :header: "関数", "説明"

   "``operator*(const mat4f &)``", "行列の積"
   "``transformPoint(v)``", "点の変換 (w = 1、除算なし)"
   "``transformPoint4(v, wOut)``", "点の変換。w も返す"
   "``transformDir(v)``", "方向ベクトルの変換 (平行移動を無視)"

集成体なので ``{{1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  tx, ty, tz, 1}}`` のように列優先で直接初期化できます。
