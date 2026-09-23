導入
################################################################################

必要なもの
================================================================================

- **C++17 対応コンパイラ** (GCC、Clang、Emscripten で確認)。
  ライブラリのソースだけでなく、ヘッダをインクルードする利用側のコードも C++17 でコンパイルしてください。
- ビルドに CMake 3.13 以降を使う場合は CMake。使わなくても構いません。
- ツール (``bin/``) とドキュメント生成には Python 3 と ``requirements.txt`` の依存パッケージ。

CMake で使う
================================================================================

自分のプロジェクトから ``add_subdirectory`` で取り込み、ターゲット ``shapoco::gfx`` にリンクします。
Pico SDK のプロジェクトでも同じです。

.. code-block:: cmake

   add_subdirectory(path/to/shapo-gfx)
   target_link_libraries(your_target PRIVATE shapoco::gfx)

ライブラリ単体でビルドしてサンプルとテストを実行するには次のようにします。

.. code-block:: sh

   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ./build/example/wasm/demo2d/demo2d out2d.ppm   # 1 フレームを PPM に書き出す
   ./build/example/wasm/demo3d/demo3d out3d.ppm
   ctest --test-dir build --output-on-failure

.. csv-table:: CMake オプション
   :header: "オプション", "既定値", "説明"

   "``SHAPOGFX3D_CORRECT_PERSPECTIVE``", "空 (ライブラリ既定の 1)", "透視補正レベル 0 / 1 / 2 (:doc:`../gfx3d/concepts` 参照)"
   "``SHAPOGFX_BUILD_EXAMPLES``", "トップレベル時 ON", "ネイティブ版サンプルをビルドする"
   "``SHAPOGFX_BUILD_TESTS``", "トップレベル時 ON", "テストをビルドする"

PlatformIO で使う
================================================================================

リポジトリのルートに ``library.json`` があるので、PlatformIO のライブラリとしてそのまま利用できます。

.. code-block:: ini

   [env:my_board]
   platform = espressif32
   board = seeed_xiao_esp32s3
   framework = arduino
   lib_deps = https://github.com/shapoco/shapo-gfx.git
   ; ヘッダが C++17 を要求する。多くのコアは既定が gnu++11 のため上書きする
   build_unflags = -std=gnu++11
   build_flags = -std=gnu++17

``lib_deps`` にはブランチやタグ (``...git#v0.1.0``)、ローカルパス (``symlink://../shapo-gfx``) も指定できます。

``build_unflags`` / ``build_flags`` は **利用側のコード** のためのものです。
ライブラリ自身のソースは ``library.json`` が ``-std=gnu++17`` を指定するので、これがなくてもビルドできますが、
ヘッダをインクルードする側のコードは C++17 でコンパイルする必要があります
(C++17 未満の場合は ``config.hpp`` が分かりやすいエラーメッセージを出します)。
既定の規格はコアによって異なるため、``build_unflags`` には ``-std=gnu++11 -std=gnu++14`` のように
複数列挙しても構いません (存在しないフラグは無視されます)。

ピクセルフォーマットの無効化などのコンパイル時オプションも ``build_flags`` に書きます。

.. code-block:: ini

   build_flags =
       -std=gnu++17
       -D SHAPOGFX_FORMAT_GRAY1=0
       -D SHAPOGFX_FORMAT_RGB444=0

CMake も PlatformIO も使わない
================================================================================

``include/`` をインクルードパスに加え、``src/gfx2d/*.cpp`` と ``src/gfx3d/*.cpp`` を C++17 でコンパイルします。
コンパイル時オプションは全て単純なマクロです。

コンパイル時オプション
================================================================================

.. csv-table::
   :header: "マクロ", "既定値", "効果"

   "``SHAPOGFX_FORMAT_GRAY1`` / ``SHAPOGFX_FORMAT_RGB444`` / ``SHAPOGFX_FORMAT_ARGB4444`` / ``SHAPOGFX_FORMAT_RGB565_SWAPPED``", "1", "0 にするとそのピクセルフォーマットのコードを 2D・3D 両方から除去する。無効化したフォーマットの Surface / Texture は無視される"
   "``SHAPOGFX_FORMAT_RGB565``", "0", "1 でネイティブバイト順の RGB565 を有効にする"
   "``SHAPOGFX3D_CORRECT_PERSPECTIVE``", "1", "テクスチャ座標の透視補正レベル (``gfx3d.cpp`` のコンパイルにのみ影響)"
   "``SHAPOGFX3D_PERSPECTIVE_STEP``", "16", "レベル 2 でテクスチャ座標を正確に求める間隔 (ピクセル、2 の冪。``gfx3d.cpp`` のみ)"
   "``SHAPOGFX3D_GOURAUD_STEP``", "1", "テクスチャ付き・グーロー補間の線分で、テクセルに乗じる頂点色を更新する間隔 (ピクセル、16 以下の 2 の冪)。4 にするとテクスチャ付きピクセルの処理が少し軽くなり、色は 4 ピクセル単位で一定になる (``gfx3d.cpp`` のみ)"
   "``SHAPOGFX_COORD_BITS``", "11", "スクリーン座標と Surface の幅・高さのビット数 (1〜15)。``2^bits - 1`` ピクセルを超える Surface は 2D の描画先として拒否され、3D の ``init()`` は失敗する。全翻訳単位で同じ値にすること"
   "``SHAPOGFX3D_RP2_INTERP``", "RP2 で 1、他は 0", "RP2040 / RP2350 (Pico SDK) で 16 ビットテクセルの参照とグーロー補間の色の歩進に SIO interpolator (``interp0`` / ``interp1``) を使う。RP2 と判定され (``PICO_RP2040`` / ``PICO_RP2350``)、``hardware/interp.h`` が見えるとき既定で有効。``hardware_interp`` のリンクが必要。0 で無効 (``gfx3d.cpp`` のみ)"
   "``SHAPOGFX_ARCH_SPLIT_MUL64``", "Cortex-M0/M0+ と ESP8266 で 1、他は 0", "1 にすると 32x32→64 ビットの積をライブラリ呼び出しでなく 16x16 ビットの積 4 つで作る (乗算器が下位 32 ビットしか出さないコア向け。固定小数点の頂点段・セットアップ・透視補正除算)。結果は同じ"
   "``SHAPOGFX3D_DEPTH_BITS``", "32", "レコードの深度の精度 (32 または 16)。16 で深度付きレコードが 4 バイト小さくなる (``gfx3d.cpp`` のみ)"
   "``SHAPOGFX3D_TEXTURE``", "1", "0 でテクスチャ/環境マッピングを除去 (``gfx3d.cpp`` のみ)"
   "``SHAPOGFX3D_GOURAUD``", "1", "0 でフラットシェーディングになる (``gfx3d.cpp`` のみ)"
   "``SHAPOGFX3D_BLEND``", "1", "0 で半透明を除去し、全て不透明に描く (``gfx3d.cpp`` のみ)"
   "``SHAPOGFX3D_LINES`` / ``SHAPOGFX3D_POINTS``", "1", "0 で線分 / 点のプリミティブを除去 (``gfx3d.cpp`` のみ)"
   "``SHAPOGFX3D_STACK_DEPTH``", "16", "行列スタックの段数 (``gfx3d.cpp`` のみ)"
   "``SHAPOGFX3D_VCACHE_SIZE``", "64", "頂点キャッシュのエントリ数 (2 の冪。``gfx3d.cpp`` のみ)"
   "``SHAPOGFX3D_LAYER_MAX``", "8", "1 シーンに持てるレイヤ数 (1〜128。``gfx3d.cpp`` のみ)"

``gfx3d.cpp`` のみに影響するマクロは公開型を変えないので、翻訳単位ごとに食い違っても壊れません。
機能を無効にするとそのコードと作業メモリが減り、同じアリーナにより多くの形状を保持できます
(:doc:`../gfx3d/concepts` の「省略できる機能」参照)。

3D レンダラの出力フォーマット 1 つにつき、ピクセルループが約 14 KB (Cortex-M33) / 20 KB (Cortex-M0+) 増えます。
このため ``RGB565`` (ネイティブ順) は既定で無効です。``SHAPOGFX_FORMAT_RGB565=1`` で使う場合、``RGB565_SWAPPED`` を使わないなら ``SHAPOGFX_FORMAT_RGB565_SWAPPED=0`` にしてください。

フォーマットのマクロと ``SHAPOGFX_COORD_BITS`` は、ヘッダを含む全ての翻訳単位で同じ値にしてください
(CMake のオプションで指定した ``SHAPOGFX_COORD_BITS`` はライブラリの利用側にも伝わります)。

プラットフォーム別の設定
================================================================================

ライブラリはアーキテクチャに依存しませんが、次の設定が各ターゲットに向いています
(クロスコンパイルとホストでの検証によるもので、実機では未確認です)。

.. csv-table::
   :header: "ターゲット", "推奨"

   "RP2350 (Cortex-M33 + FPU)", "既定の float ビルド。``SHAPOGFX3D_RP2_INTERP`` は ``hardware_interp`` があれば既定で有効。``SHAPOGFX3D_HOT_ATTR`` と ``SHAPOGFX3D_HOT_INSTANTIATE=1`` でラスタライズ側を RAM に置き、``Config::renderContexts = 2`` で 2 コア描画。RISC-V (Hazard3) モードでは FPU がないので ``SHAPOGFX3D_FIXED_POINT=1``"
   "RP2040 (Cortex-M0+、FPU なし)", "``SHAPOGFX3D_FIXED_POINT=1``、ラスタライズ側を RAM に、2 コア描画。XIP フラッシュ上のテクスチャはコードと 16 KB のキャッシュを奪い合うので、小さくよく使うテクスチャは RAM へコピーする。使わない 16 ビット出力フォーマットは無効化 (RAM 上のコードが約 20 KB 減る)。メモリや速度が足りなければ ``SHAPOGFX3D_DEPTH_BITS=16``、``SHAPOGFX3D_GOURAUD_STEP=4``"
   "ESP32-S3 (Xtensa LX7 + FPU)", "float ビルド。アリーナは PSRAM ではなく内部 SRAM に置く。``RGB565`` に描いて ``esp_lcd`` 側でバイトスワップし、2 つのレンダリングコンテキストで 2 コア描画 (各コアに固定したタスクから)"
   "ESP32-P4 (RISC-V + FPU、2 コア)", "S3 と同様。アリーナは内部メモリ (L2MEM) に。大きな 2D の転送や塗りはアプリ側で PPA / 2D-DMA に任せられる (3D レンダラでは使えない)"

最小のサンプル: 2D
================================================================================

.. code-block:: cpp

   #include "shapoco/gfx2d/gfx2d.hpp"
   #include "shapoco/gfx2d/fonts.hpp"

   namespace g2 = shapoco::gfx2d;

   static uint16_t fb[320 * 240];  // RGB565_SWAPPED のフレームバッファ
   static const g2::Surface screen = {g2::PixelFormat::RGB565_SWAPPED, 320, 240, 320 * 2, fb};

   void draw() {
     g2::Graphics2D g(screen);
     g.clear(g2::makeColor(20, 24, 40));
     g.fillRoundRect(20, 20, 200, 100, 12, g2::makeColor(255, 255, 255, 40));  // 半透明
     g.drawCircle(260, 120, 40, g2::Colors::CYAN);
     g.setFont(&ShapoSansP_s12c09a01w02);
     g.setTextColor(g2::Colors::WHITE);
     g.drawString(32, 32, "Hello, ShapoGFX");
     // ... fb をディスプレイへ転送 ...
   }

最小のサンプル: 3D
================================================================================

.. code-block:: cpp

   #include "shapoco/gfx3d/gfx3d.hpp"

   namespace g2 = shapoco::gfx2d;
   namespace g3 = shapoco::gfx3d;

   static uint8_t arena[64 * 1024];   // 3D レンダラの作業メモリ
   static uint16_t band[320 * 40];    // 40 ライン分の転送バッファ (RGB565_SWAPPED)
   static const g2::Surface bandSurface = {g2::PixelFormat::RGB565_SWAPPED, 320, 40, 320 * 2, band};
   static g3::Graphics3D g3d;

   static const g3::Material matRed = {
       {0.9f, 0.15f, 0.1f, 1.0f}, {0.9f, 0.15f, 0.1f, 1.0f}, nullptr, g3::BlendMode::NONE, 0,
   };

   void setup() {
     g3d.init(320, 240, arena, sizeof(arena));
     g3d.setPerspectiveProjection(60.0f * 3.14159f / 180.0f, 320.0f / 240.0f, 0.3f, 100.0f);
     g3d.setClearColor({0.05f, 0.05f, 0.1f, 1.0f});
   }

   void drawFrame(float t) {
     g3d.beginScene();
     g3d.lookAt({0, 2, 5}, {0, 0, 0});          // カメラ
     g3d.enableParallelLight({-0.5f, -1, -0.6f}, {1, 1, 1, 1});
     g3d.enableEnvironmentLight({0.2f, 0.2f, 0.3f, 1});
     g3d.pushState();
     g3d.rotate(t, 0.3f, 1, 0);
     g3d.setMaterial(matRed);
     g3d.putCube({0, 0, 0}, {1.5f, 1.5f, 1.5f});
     g3d.popState();
     g3d.putTorus({0, -1.5f, 0}, 1.5f, 0.3f);
     g3d.endScene();

     g3d.beginRender();
     for (int y = 0; y < 240; y += 40) {
       g3d.render(0, y, 320, 40, bandSurface);  // 画面の領域 -> band の (0, 0)
       // ... band をディスプレイの (0, y) へ転送 ...
     }
     g3d.endRender();
   }

3D シーンの構築 (``beginScene()`` 〜 ``endScene()``) では頂点処理まで行い、``render()`` で領域ごとにラスタライズします。
そのためフレームバッファを持たずに帯状の転送が可能です。詳しくは :doc:`../gfx3d/concepts` を参照してください。

ツールとドキュメントの依存パッケージ
================================================================================

.. code-block:: sh

   python3 -m pip install -r requirements.txt

``bin/requirements.txt`` (Pillow, numpy, pygltflib) と Sphinx 関連が入ります。
``bin/`` のツールはインライン依存宣言 (PEP 723) を持つので ``uv run bin/img2cpp ...`` でも実行できます。
