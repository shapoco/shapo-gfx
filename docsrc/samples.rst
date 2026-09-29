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

- `demorig <../example/demorig/>`__: DragonBones のキャラクタ (``dbones2cpp`` で変換) を ``rig::Instance`` で動かします。
  24 fps のアニメーションを毎フレーム補間し、上下に揺れるキャラクタの周りを加算合成の矩形のリングが回ります。
  リングの奥半分、キャラクタの左腕より奥 (``draw(g, 0, k)``)、リングの手前半分、キャラクタの残り (``draw(g, k, n)``)
  の順に描くので、左腕だけがリングの手前に出ます。背景では、demo2d と同じ線と塗りつぶしのカラフルな星が
  回転しながら斜めに降ります。右端の (+) / (-) ボタンで 2 倍ずつズームイン / ズームアウト (1/4 〜 16 倍、滑らかに変化)、
  それ以外の部分をドラッグ (タッチパネルならスワイプ) するとスクロールします。左上にフレームレートと倍率を表示します。
  URL に ``?screen=WxH`` を付けると画面サイズを変えられます (例: `320x240 <../example/demorig/?screen=320x240>`__、
  既定は 480x320)。

いずれも 480x320 (demorig は指定したサイズ) の RGB565_SWAPPED バッファに描画し、``docs/example/viewer.js`` が RGB565_SWAPPED をキャンバスに展開しています。

ソース
================================================================================

.. csv-table::
   :header: "パス", "内容"

   "``example/wasm/demo2d/``", "``scene.cpp`` (描画)、``main.cpp`` (WASM エクスポートとネイティブ ``main()``)、``Makefile``、``CMakeLists.txt``"
   "``example/wasm/demo3d/``", "同上。``model/`` に風車の生成スクリプト、``.glb``、生成ヘッダ"
   "``example/wasm/demorig/``", "``main.cpp`` (WASM エクスポートとネイティブ ``main()``)、``Makefile``、``CMakeLists.txt``"
   "``example/common/demorig/``", "demorig のシーン (``scene.cpp``) とビュー・ボタン・FPS 表示 (``demorig.cpp``)。ShapoGFX だけに依存し、WASM 版と M5Stack 版で共有。``model/rgb_chan.hpp`` は ``make -C example/wasm/demorig model`` で ``assets/2d/rgb_chan/`` から生成"
   "``example/m5cores3/demorig/`` ほか", "M5Stack 版 demorig の ESP-IDF プロジェクト (下記)"
   "``example/m5common/``", "M5Stack 版の共通コンポーネント (ShapoGFX、demorig の共通コード、M5Unified を使うフロントエンド)"
   "``docs/example/``", "``viewer.js`` (共通ビューア)、``style.css``、各デモの ``index.html`` と ``.wasm``"

ネイティブ版は 1 フレームを PPM ファイルに書き出します (ブラウザなしで動作確認するため)。

.. code-block:: sh

   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
   ./build/example/wasm/demo3d/demo3d frame.ppm 1.5    # 第 2 引数は経過秒
   # demorig: 経過秒、画面サイズ、倍率、画面中央に来るシーン上の点、バンド数
   ./build/example/wasm/demorig/demorig frame.ppm 1.5 320x240 16 160 120 5

WASM のビルド
================================================================================

`Emscripten <https://emscripten.org/>`__ が必要です。

.. code-block:: sh

   (cd example/wasm/demo2d && make)   # docs/example/demo2d/demo2d.wasm
   (cd example/wasm/demo3d && make)   # docs/example/demo3d/demo3d.wasm
   (cd example/wasm/demorig && make)  # docs/example/demorig/demorig.wasm
   ./launch_web_server.sh             # docs/ を http://localhost:52880/ で配信

``fetch()`` を使うため ``file://`` では動きません。WASM バイナリはリポジトリにコミットされており、
``docs/`` を GitHub Pages でそのまま公開できます。

M5Stack 版 demorig
================================================================================

demorig は M5Stack CoreS3 と Tab5 でも動きます。ESP-IDF 5.5 のプロジェクトで、画面の初期化とタッチパネルに
M5Unified / M5GFX を使います (依存コンポーネントは初回ビルド時にダウンロードされます)。
ShapoGFX はリポジトリ自身をコンポーネントとして取り込みます (:doc:`intro/getting_started` の「ESP-IDF で使う」参照)。

.. csv-table::
   :header: "機種", "プロジェクト", "画面", "操作"

   "M5Stack CoreS3", "``example/m5cores3/demorig/``", "320x240 (SPI 40 MHz)", "ボタンとスワイプ"
   "M5Stack Tab5", "``example/m5tab5/demorig/``", "640x360 に描画し PPA で 2 倍に拡大して 1280x720 へ", "ボタンとスワイプ"

.. code-block:: sh

   cd example/m5cores3/demorig
   ./build.sh          # ビルド (ESP-IDF は IDF_ROOT、既定 ~/esp/5.5)
   ./run.sh [PORT]     # ビルドして書き込み
   ./monitor.sh [PORT] # シリアルログ

フレームは 60 行のストリップ単位で描画します。1 つのストリップの上半分をコア 0、
下半分をコア 1 が同時に描き、描き終えたストリップを SPI DMA (Tab5 は PPA) で送る間に次のストリップを
もう一方のバッファに描きます。シリアルログには 2 秒ごとにフレームレート、倍率、1 フレームあたりの時間
(シーンの更新、コア 0 / コア 1 の描画、コア 1 の待ち、パネル転送の待ち) が出ます。
``idf.py -DM5DEMORIG_DUAL_CORE=0 build`` でビルドすると、コア 0 だけで描画します (比較用、元に戻すには ``-DM5DEMORIG_DUAL_CORE=1``)。
M5Stack 版はアトラスではなく画像ごとのテクスチャ (``model/rgb_chan_sep.hpp``、``--atlas-width 0``) を使います。
パーツはフラッシュからキャッシュ経由で読まれ、行に余白のないぶんフレームあたりのキャッシュラインが 3 分の 1 減るためです
(rgb_chan で 64 バイト 5133 ラインに対して 3464)。``scene.cpp`` は ``DEMORIG_MODEL_HEADER`` が定義されていればそのヘッダを使います。

CoreS3 は SPI 40 MHz で全画面を送るのに約 31 ms かかるので、フレームレートの上限は約 32 fps です。
実測は等倍で CoreS3 24 fps、Tab5 42 fps です (2026-09-27、``--scale 0.4 --fit-rotate`` を 2 倍解像度の
アセットに適用し、M5Stack 版は ``--atlas-width 0``)。経緯: ``--hull`` の前は CoreS3 23 fps / Tab5 26 fps、
アトラスのモデルでは Tab5 が 30 fps。``--out-format auto`` (tie と bracelet 以外をキーカラー) では
CoreS3 27 fps / Tab5 56 fps になりましたが、縁の画質が落ちすぎるので demorig では採用していません。
Tab5 の伸びが大きいのは、パーツ全体が L2 キャッシュ (256 KB) に収まるかどうかで決まっているためです。
オプションの効き方は :doc:`../tools/dbones2cpp` の「性能のためのオプションの選び方」を参照してください。キャラクタを拡大すると、重なったパーツの描画面積が増えるため描画が重くなります
(320x240、x86-64 の実行命令数で、等倍の約 380 万に対して 2 倍以上では約 840 万)。

新しいデモを作る
================================================================================

``example/wasm/`` の既存デモをコピーし、``Makefile`` の ``demo3d`` を新しい名前に置き換えます。
エクスポート関数は ``<name>_init``, ``<name>_frame(t[, yaw, pitch, dist])``, ``<name>_get_fb``,
``<name>_get_width``, ``<name>_get_height`` の 5 つで、``docs/example/<name>/index.html`` から
``startDemoViewer({wasm, prefix, camera})`` を呼ぶだけです。
``startDemoViewer`` に ``screenQuery: true`` を渡すと URL の ``?screen=WxH`` を ``<name>_set_screen(w, h)`` に、
``pointer: true`` を渡すとマウス / タッチを ``<name>_pointer_down(x, y)``, ``<name>_pointer_move(x, y)``,
``<name>_pointer_up()`` に渡します (demorig が使っています)。
