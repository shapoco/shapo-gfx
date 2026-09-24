サンプル
################################################################################

ブラウザで動くデモ
================================================================================

- `demo2d <../example/demo2d/>`__: ``Graphics2D`` の主な機能 (背景パターン、多角形の星、α と加算合成のスプライト、
  GRAY1 アイコン、RGB444 オフスクリーンの転送とカラーキー、拡大・鏡像・回転した画像と、一緒に回転する枠と文字、
  図形、円グラフ、クリップ、複数のフォントと変換行列による拡大)。``shapoco::gfx2d`` だけを使います。
- `demo3d <../example/demo3d/>`__: テクスチャ付きの床、環境マップのトーラス (``putTorus``)、不透明・半透明・加算合成のキューブ、
  glTF から生成した頂点カラーの風車 (``putScene`` と ``NodeVisitor``)。``Graphics2D`` で描いた背景の上に、
  背景クリアを無効にして 3D を重ねています。マウスドラッグ / 矢印キーで回転、ホイール / PageUp・PageDown でズームします。

どちらも 480x320 の RGB565_SWAPPED バッファに描画し、``docs/example/viewer.js`` が RGB565_SWAPPED をキャンバスに展開しています。

ソース
================================================================================

.. csv-table::
   :header: "パス", "内容"

   "``example/wasm/demo2d/``", "``scene.cpp`` (描画)、``main.cpp`` (WASM エクスポートとネイティブ ``main()``)、``Makefile``、``CMakeLists.txt``"
   "``example/wasm/demo3d/``", "同上。``model/`` に風車の生成スクリプト、``.glb``、生成ヘッダ"
   "``docs/example/``", "``viewer.js`` (共通ビューア)、``style.css``、各デモの ``index.html`` と ``.wasm``"

ネイティブ版は 1 フレームを PPM ファイルに書き出します (ブラウザなしで動作確認するため)。

.. code-block:: sh

   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
   ./build/example/wasm/demo3d/demo3d frame.ppm 1.5    # 第 2 引数は経過秒

WASM のビルド
================================================================================

`Emscripten <https://emscripten.org/>`__ が必要です。

.. code-block:: sh

   (cd example/wasm/demo2d && make)   # docs/example/demo2d/demo2d.wasm
   (cd example/wasm/demo3d && make)   # docs/example/demo3d/demo3d.wasm
   ./launch_web_server.sh             # docs/ を http://localhost:52880/ で配信

``fetch()`` を使うため ``file://`` では動きません。WASM バイナリはリポジトリにコミットされており、
``docs/`` を GitHub Pages でそのまま公開できます。

新しいデモを作る
================================================================================

``example/wasm/`` の既存デモをコピーし、``Makefile`` の ``demo3d`` を新しい名前に置き換えます。
エクスポート関数は ``<name>_init``, ``<name>_frame(t[, yaw, pitch, dist])``, ``<name>_get_fb``,
``<name>_get_width``, ``<name>_get_height`` の 5 つで、``docs/example/<name>/index.html`` から
``startDemoViewer({wasm, prefix, camera})`` を呼ぶだけです。
