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
   "``RectF { float x, y, width, height; }``", "``Rect`` の float 版 (連続座標)。``Rect`` から暗黙変換できる。``right()``, ``bottom()``, ``isEmpty()``, ``normalized()``, ``offset(dx, dy)``"
   "``affine2f { float a, b, c, d, tx, ty; }``", "2D アフィン変換。下記"

affine2f
--------------------------------------------------------------------------------

.. code-block:: cpp

   struct affine2f {  // x' = a x + c y + tx,  y' = b x + d y + ty
     float a, b, c, d, tx, ty;
     static affine2f identity();
     static affine2f translation(float x, float y);
     static affine2f scaling(float sx, float sy);
     static affine2f scaling(float s);
     static affine2f rotation(float angle);                  // 原点まわり
     static affine2f rotation(float angle, float cx, float cy);  // (cx, cy) まわり
     static affine2f shearing(float kx, float ky);           // x' = x + kx y, y' = ky x + y
     // 画像の点 (pivotX, pivotY) を (x, y) へ。(sx, sy) 倍して、その点のまわりに angle 回す
     static affine2f placement(float x, float y, float angle, float sx = 1,
                               float sy = 1, float pivotX = 0, float pivotY = 0);

     // 右から掛ける (後に書いたものが先に効く。canvas のコンテキストと同じ)
     affine2f &translate(float x, float y);
     affine2f &scale(float sx, float sy);
     affine2f &scale(float s);
     affine2f &rotate(float angle);
     affine2f &shear(float kx, float ky);
     affine2f &multiply(const affine2f &m);

     vec2f apply(float x, float y) const;  vec2f apply(const vec2f &) const;
     vec2f applyLinear(const vec2f &) const;  // 平行移動なし (方向ベクトル用)
     float determinant() const;
     bool invert(affine2f &out) const;       // 逆変換。特異なら false
   };
   affine2f operator*(const affine2f &m, const affine2f &n);  // n の後に m
   vec2f operator*(const affine2f &m, const vec2f &p);

座標は連続値で、ピクセル (x, y) は ``[x, x + 1) x [y, y + 1)`` を覆います (``translation(10, 20)`` で画像の左上角が
ピクセル (10, 20) の左上角に来る)。角度はラジアンで、y 軸が下向きなので正の角度は画面上で時計回りです。

.. code-block:: cpp

   // 画像を (x, y) を中心に、中心のまわりに回転・拡大して置く
   g2::affine2f m = g2::affine2f::translation(x, y);
   m.rotate(angle).scale(2.0f).translate(-w * 0.5f, -h * 0.5f);
   // 同じもの
   g2::affine2f m2 = g2::affine2f::placement(x, y, angle, 2, 2, w * 0.5f, h * 0.5f);

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
