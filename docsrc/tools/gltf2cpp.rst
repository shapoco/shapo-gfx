gltf2cpp: glTF モデルを C++ コードに変換する
################################################################################

``bin/gltf2cpp`` は glTF 2.0 (``.gltf`` / ``.glb``) を、``Graphics3D::putScene()`` で描ける
``static const`` データ一式のヘッダに変換します。実行時のメモリ確保も RAM 消費もありません。

.. code-block:: sh

   python3 -m pip install -r bin/requirements.txt      # Pillow, numpy, pygltflib
   bin/gltf2cpp robot.glb robot.hpp                     # 名前空間 robot
   bin/gltf2cpp --namespace assets --texformat argb4444 --dither diffusion model.gltf model.hpp

オプション
================================================================================

.. csv-table::
   :header: "オプション", "既定値", "説明"

   "``--namespace``", "入力ファイル名", "生成物を囲む名前空間"
   "``--texformat``", "``auto``", "テクスチャの形式。``auto`` は画像やマテリアルが α を使うなら ``argb4444``、それ以外は ``rgb565be``"
   "``--dither``", "``none``", "テクスチャのディザ (``none`` / ``diffusion`` / ``pattern``)"
   "``--key-color``", "なし", "テクスチャで透明にする色"
   "``--max-texture-size N``", "なし", "N より大きいテクスチャを縮小する"
   "``--no-resize-pot``", "オフ", "2 の冪へのリサイズを行わず警告だけ出す"

対応する glTF の機能
================================================================================

.. csv-table::
   :header: "項目", "扱い"

   "プリミティブモード", "TRIANGLES / TRIANGLE_STRIP / TRIANGLE_FAN。POINTS / LINES は警告して無視"
   "POSITION", "必須"
   "NORMAL", "なければ面法線を頂点に累積して生成"
   "TEXCOORD_0", "UV (それ以外の UV セットは無視)"
   "COLOR_0", "``Vertex::color`` に格納し、マテリアルに ``VERTEX_COLOR`` を立てた複製 (``mat<i>Vc``) を使う"
   "添字", "``uint16_t``。頂点数 65535 超、範囲外の添字は警告してそのプリミティブを飛ばす。非インデックスは連番を生成"
   "baseColorFactor", "``diffuse`` と ``ambient``"
   "baseColorTexture", "``Texture`` (画像単位で共有)。2 の冪でなければ最も近い 2 の冪にリサイズ (警告)"
   "alphaMode", "BLEND → ``BlendMode::ALPHA``。MASK は ARGB4444 テクスチャのテクセル α で表現 (なければ不透明で警告)"
   "doubleSided", "``MaterialFlags::DOUBLE_SIDED``"
   "ノードの変換", "TRS (四元数含む) または matrix をツール側で列優先の 4x4 に合成"
   "シーン", "全シーンと、既定シーンの別名 ``scene``"
   "未対応", "スキン、アニメーション、カメラ、ライト、KHR 拡張 (テクスチャ変形など)"

出力の構成
================================================================================

名前空間の中に次の名前で ``static const`` オブジェクトが並びます。

.. csv-table::
   :header: "名前", "内容"

   "``tex<i>Data``, ``tex<i>``", "画像 i のピクセル配列と ``Texture``"
   "``mat<i>``, ``mat<i>Vc``", "マテリアル i (``Vc`` は頂点色付きプリミティブ用の複製)。マテリアルなしは ``matDefault``"
   "``mesh<i>Prim<j>Vertices`` / ``...Vb`` / ``...Indices``", "プリミティブの頂点配列、``VertexBuffer``、添字配列"
   "``mesh<i>Prims``, ``mesh<i>``", "プリミティブ配列と ``Mesh``"
   "``node_<名前>`` または ``node<i>``", "ノード。glTF の名前が識別子として使えて重複しなければ名前付き。子は親より前に定義される"
   "``scene<i>``, ``scene``", "シーンと既定シーンの参照"

.. code-block:: cpp

   #include "windmill.hpp"

   g3d.putScene(windmill::scene);              // 全体
   g3d.putNode(windmill::node_Blades);         // 部分木だけ
   const g3::Mesh *m = windmill::node_Tower.mesh;

ノード名で部品をアニメーションさせる方法は :doc:`../gfx3d/scene` の ``NodeVisitor`` を参照してください。

サイズの目安
================================================================================

頂点 1 個は 36 バイト (位置 12、法線 12、UV 8、色 4)、添字は 2 バイトです。
ツールは末尾のコメントに頂点数・添字数・テクスチャ数と概算バイト数を出力します。

ワークフローの例
================================================================================

demo3d の風車は次の手順で作られています。

.. code-block:: sh

   python3 example/wasm/demo3d/model/make_windmill.py                 # windmill.glb を生成 (Blender 等の出力でも同じ)
   bin/gltf2cpp --namespace windmill example/wasm/demo3d/model/windmill.glb \
                example/wasm/demo3d/model/windmill.hpp

Blender からエクスポートする場合は「glTF Binary (.glb)」で、テクスチャは埋め込み、
+Y up (既定) のままにしてください。
