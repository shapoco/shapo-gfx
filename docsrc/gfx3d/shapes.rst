基本形状
################################################################################

``Graphics3D`` には基本形状を直接描く関数があります。全て現在のマテリアルで描かれ、
毎回その場で頂点を生成して ``putPrimitive()`` に渡します。

共通の規約
================================================================================

- ``center`` を中心とし、軸は **+Y** です。向きを変えたい場合は ``rotate()`` を使います。
- 法線は外向き、表面は反時計回りです。片面マテリアルで外側が見えます。
- 分割数は 3〜64 (細分化数は 1〜64、イコスフィアのレベルは 0〜4) にクランプされます。
- 頂点色は白 (``VERTEX_WHITE``) です。

関数一覧
================================================================================

.. csv-table::
   :header: "関数", "説明", "UV"

   "``putCube(center, size, divs = 1)``", "直方体。``divs`` は各面の一辺あたりの分割数", "面ごとに (0,0)〜(1,1)"
   "``putPlane(center, sizeX, sizeZ, divsX = 1, divsZ = 1)``", "XZ 平面の矩形 (+Y 向き、片面)", "u は +X、v は +Z 方向"
   "``putDisk(center, radius, segments = 16)``", "XZ 平面の円盤 (+Y 向き、片面)", "外接正方形に対応"
   "``putSphereUV(center, radius, segmentsU = 16, segmentsV = 8)``", "緯度経度球", "u = 経度 (+X から +Z 方向へ)、v = 0 が北極 (+Y)、1 が南極"
   "``putIcosphere(center, radius, level = 2)``", "正二十面体を細分化した球 (20 x 4^level 三角形)", "putSphereUV と同じ正距円筒図法。継ぎ目と極は面ごとに補正"
   "``putCylinder(center, radius, height, segments = 16, heightDivs = 1, caps = true)``", "円柱", "側面: u = 周方向、v = 0 が上端、1 が下端。蓋は円盤と同じ"
   "``putCone(center, radiusBottom, radiusTop, height, segments = 16, heightDivs = 1, caps = true)``", "円錐台。``radiusTop = 0`` で円錐、両半径が同じなら円柱", "putCylinder と同じ"
   "``putTorus(center, majorRadius, minorRadius, majorSegments = 24, minorSegments = 12)``", "トーラス", "u = 大円周方向、v = 管の周方向"

.. code-block:: cpp

   g3d.setMaterial(matChrome);
   g3d.pushState();
   g3d.translate(0, 0.5f, 0);
   g3d.rotate(t, 0, 1, 0);
   g3d.putTorus({0, 0, 0}, 1.0f, 0.35f, 24, 12);
   g3d.popState();

   g3d.setMaterial(matWood);
   g3d.putCylinder({2, 0, 0}, 0.3f, 2.0f, 12);       // 柱
   g3d.putCone({2, 1.3f, 0}, 0.6f, 0.0f, 0.8f, 12);  // 屋根

実装とコスト
================================================================================

パラメトリックな面 (板、球、円柱、円錐、トーラス) は緯度帯ごとに、最大 16 セグメントのチャンクに分けて
``TRIANGLE_STRIP`` として投入されます。頂点は 34 個分のスタックバッファ (約 1.1 KB) に生成されるだけで、
形状の大きさに比例したメモリは使いません。三角関数は呼び出しごとに分割数分だけ表を作ってから頂点を求めるので、
毎フレーム形状を生成し直しても、あらかじめ作ったメッシュを描くのと大きな差はありません
(チャンク境界の頂点が 2 回シェーディングされる程度です)。

イコスフィアは正二十面体の各面を細分化し、面ごとに行単位のストリップで投入します。
UV は正距円筒図法で、面が経度 0 の継ぎ目をまたぐ場合は u を連続にし (テクスチャ座標は自動的にラップします)、
極の頂点には面の中心の u を与えます。

三角形数の目安:

.. csv-table::
   :header: "形状", "三角形数"

   "putCube(divs)", "12 x divs²"
   "putSphereUV(u, v)", "2 x u x v (極では縮退)"
   "putIcosphere(level)", "20 x 4^level (level 2 で 320)"
   "putCylinder / putCone(segments, heightDivs, caps)", "2 x segments x heightDivs + 蓋 2 x segments"
   "putTorus(major, minor)", "2 x major x minor"

アリーナの三角形容量 (``Stats::triCapacity``) を超えないように分割数を選んでください。
