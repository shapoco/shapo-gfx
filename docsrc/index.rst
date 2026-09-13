ShapoGFX Reference Manual
################################################################################

ShapoGFX は、小さなディスプレイを駆動するマイクロコントローラ向けの 2D/3D グラフィックスライブラリです。
依存ライブラリのない C++17 で書かれており、フレームバッファも Z バッファも持たない
スキャンライン方式の 3D レンダラと、ビットマップフォント付きの 2D 描画 API を提供します。

- GitHub: `github.com/shapoco/shapo-gfx <https://github.com/shapoco/shapo-gfx>`__
- ブラウザで動くデモ: `demo2d <../example/demo2d/>`__ / `demo3d <../example/demo3d/>`__
- 英語の設計仕様書: `SPEC.md <https://github.com/shapoco/shapo-gfx/blob/main/SPEC.md>`__

.. toctree::
   :maxdepth: 2
   :caption: はじめに

   intro/overview.rst
   intro/getting_started.rst

.. toctree::
   :maxdepth: 2
   :caption: 2D グラフィックス (shapoco::gfx2d)

   gfx2d/pixel.rst
   gfx2d/surface.rst
   gfx2d/graphics2d.rst
   gfx2d/fonts.rst

.. toctree::
   :maxdepth: 2
   :caption: 3D グラフィックス (shapoco::gfx3d)

   gfx3d/concepts.rst
   gfx3d/graphics3d.rst
   gfx3d/shapes.rst
   gfx3d/scene.rst
   gfx3d/math.rst

.. toctree::
   :maxdepth: 2
   :caption: ツール

   tools/img2cpp.rst
   tools/gltf2cpp.rst

.. toctree::
   :maxdepth: 1
   :caption: その他

   samples.rst
