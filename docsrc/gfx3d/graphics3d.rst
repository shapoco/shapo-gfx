Graphics3D API
################################################################################

ヘッダ: ``shapoco/gfx3d/gfx3d.hpp``

データ構造
================================================================================

Vertex / VertexBuffer
--------------------------------------------------------------------------------

.. code-block:: cpp

   struct Vertex {
     vec3f position;
     vec3f normal;
     vec2f uv;            // 環境マッピング時は未使用
     gfx2d::Color color;  // ARGB8888。MaterialFlags::VERTEX_COLOR のときに使用 (α は無視)
   };
   constexpr gfx2d::Color VERTEX_WHITE = 0xFFFFFFFFu;

   // 16 バイトの圧縮頂点。フラッシュ上のモデルデータを小さくする
   struct PackedVertex {
     int16_t position[3];  // VertexBuffer::scale 倍して bias を足した値が座標
     int16_t uv[2];        // 1/1024 単位 (範囲 -32〜32)
     int8_t normal[3];     // 1/127 単位
     uint8_t color[3];     // R, G, B
   };

   struct VertexBuffer {
     uint16_t vertexCount;
     const Vertex *vertices;                // nullptr なら packed を使う
     const PackedVertex *packed = nullptr;  // 16 バイト頂点
     vec3f scale = {1, 1, 1};               // packed の座標スケール
     vec3f bias = {0, 0, 0};                // packed の座標オフセット
   };

``VertexBuffer`` は 36 バイトの ``Vertex`` と 16 バイトの ``PackedVertex`` のどちらでも持てます。
圧縮頂点の座標はプリミティブのバウンディングボックスを 65534 分割した精度、法線は約 1% の誤差で
単位長になり、どちらも出力フォーマットの分解能より十分細かい値です。
デコードは頂点ごとに 1 回だけ (頂点キャッシュが吸収する) なので、効くのはフラッシュ使用量です。
``bin/gltf2cpp --vertex-format packed`` がこの形式を出力します
(:doc:`../tools/gltf2cpp`)。

``VertexBuffer`` はどちらの形式を指す場合でも ``packed`` とスケール・オフセットを持つため、
32bit ターゲットで 36 バイトです (ポインタと個数だけなら 8 バイト)。
``Vertex`` を使うバッファはこの 28 バイトを余分に払うことになり、
圧縮頂点は 1 頂点あたり 20 バイトを節約します。
プリミティブあたり 2 頂点以上あれば圧縮したほうが小さくなりますが、
数頂点のプリミティブが多数あるモデルでは効果が小さくなります。

Texture
--------------------------------------------------------------------------------

``gfx2d::Texture`` をそのまま使います (:doc:`../gfx2d/surface`)。任意の有効フォーマットが使えますが、
幅と高さは 2 の冪でなければなりません。

Material
--------------------------------------------------------------------------------

.. code-block:: cpp

   namespace MaterialFlags {
   constexpr uint32_t TEXTURE = 1u << 0;       // テクスチャを使う
   constexpr uint32_t ENV_MAP = 1u << 1;       // テクスチャを環境マップとして使う
   constexpr uint32_t DOUBLE_SIDED = 1u << 2;  // 両面描画 (バックフェイスカリング無効)
   constexpr uint32_t VERTEX_COLOR = 1u << 3;  // Vertex::color を乗算する
   }

   struct Material {
     colorf diffuse;          // 拡散反射色。a は不透明度
     colorf ambient;          // 環境反射色
     const Texture *texture;  // 未使用なら nullptr
     BlendMode blendMode;     // NONE, ALPHA, ADD
     uint32_t flags;          // MaterialFlags の組み合わせ
   };

.. code-block:: cpp

   static const g3::Material matGlass = {
       {0.4f, 0.7f, 1.0f, 0.45f}, {0.4f, 0.7f, 1.0f, 1.0f},
       nullptr, g3::BlendMode::ALPHA, g3::MaterialFlags::DOUBLE_SIDED,
   };

Primitive
--------------------------------------------------------------------------------

.. code-block:: cpp

   enum class PrimitiveType : uint8_t {
     TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN,   // ライティング・テクスチャ・カリングあり
     POINTS, LINES, LINE_STRIP, LINE_LOOP       // ライティングなし、1 px (点は pointSize)、カリングなし
   };

点と線については :doc:`concepts` の「点と線」を参照してください。

.. code-block:: cpp

   struct Primitive {
     PrimitiveType type;
     const VertexBuffer *vertexBuffer;
     uint16_t indexCount;
     const uint16_t *indices;
     const Material *material;  // nullptr なら setMaterial() で設定したマテリアル
   };

Config / LayerFlags
--------------------------------------------------------------------------------

.. code-block:: cpp

   struct Config {
     int16_t screenWidth = 0, screenHeight = 0;
     void *arena = nullptr;   // 作業メモリ
     size_t arenaSize = 0;
     int spanCapacity = 0;    // 1 ラインに持てる線分数 (レンダリングコンテキストごと)。0 なら既定値
     int renderContexts = 1;  // 同時に実行できる render() の数 (1〜4)
   };
   Config defaultConfig(int16_t w, int16_t h, void *arena, size_t arenaSize);

   namespace LayerFlags {
   constexpr uint32_t NO_DEPTH = 1u << 0;   // 深度を持たず、投入順で前後が決まる
   }

既定値は ``defaultConfig()`` で受け取り、変えたいメンバだけ書き換えて ``init()`` に渡します。
メンバは後方互換な既定値付きで追加されることがあります。

Stats
--------------------------------------------------------------------------------

.. code-block:: cpp

   struct Stats {
     size_t arenaSize;      // init() に渡したアリーナのサイズ
     size_t arenaUsed;      // 直近フレームで実際に使った量 (固定分 + 三角形 + 線分ピーク)
     size_t triBytes;       // 三角形バッファの使用量 (レコード + エントリ 4 バイト/個)
     size_t triBytesTotal;  // 三角形バッファに使える量
     int triCount;          // 現在のシーンの三角形数 (カリング後)
     int triDropped;        // バッファあふれで破棄した数 (beginScene() でリセット)
     int layerCount;        // 現在のシーンのレイヤ数 (beginScene() でリセット)
     int layersDropped;     // 空きがなく無視した beginLayer() の数 (beginScene() でリセット)
     int spanCapacity;      // 線分プールの容量 (コンテキストごと)
     int spanPeak;          // 1 ラインで同時に使った線分数の最大、最も使ったコンテキストの値 (beginRender() でリセット)
     int spanDropped;       // プールあふれで破棄した数、全コンテキストの合計 (beginRender() でリセット)
     int badIndices;        // 添字範囲外で破棄した三角形数 (beginScene() でリセット)
     int nodesDropped;      // スタック満杯で飛ばしたノード数 (beginScene() でリセット)
   };

初期化
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void init(const Config &)``", "初期化パラメータを与える。アリーナが小さすぎる場合は未初期化のまま"
   "``void init(int16_t w, int16_t h, void *arena, size_t arenaSize)``", "``init(defaultConfig(w, h, arena, arenaSize))`` と同じ"
   "``void deinit()``", "アリーナを手放す。以降の描画呼び出しは何もしない"
   "``bool isInitialized() const``", "初期化済みか"
   "``int16_t screenWidth() const`` / ``screenHeight() const``", "画面サイズ"

シーンの構築
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void beginScene()``", "三角形バッファ、レイヤ、スタック、現在の行列をリセットして構築を始める"
   "``void endScene()``", "構築を終える"
   "``void beginLayer(uint32_t flags = 0)``", "新しいレイヤを開く。以降のプリミティブはそれまでの全てより手前に描かれる。空きがなければ無視される"
   "``void endLayer()``", "現在のレイヤを閉じる。以降のプリミティブは既定フラグの新しいレイヤに入る"
   "``void loadIdentity()``", "現在の行列を単位行列にする"
   "``void translate(const vec3f &)`` / ``translate(x, y, z)``", "平行移動を右から乗じる"
   "``void rotate(float angle, const vec3f &axis)`` / ``rotate(angle, x, y, z)``", "回転 (ラジアン、軸は正規化不要)"
   "``void scale(const vec3f &)`` / ``scale(x, y, z)``", "スケール"
   "``void transform(const mat4f &)``", "任意の行列を右から乗じる"
   "``void lookAt(const vec3f &eye, const vec3f &target, const vec3f &up = {0, 1, 0})``", "視点行列を右から乗じる (gluLookAt 相当)"
   "``bool pushState()``", "現在の行列とマテリアルをスタックに保存する。満杯なら false を返して何もしない"
   "``void popState()``", "復元する"
   "``void setMaterial(const Material &)``", "現在のマテリアルを設定する (ポインタを保持するので endRender() まで有効なオブジェクトを渡す)"
   "``void putPrimitive(const Primitive &)``", "プリミティブを追加する。頂点処理はこの時点で行われる"
   "``void setPointSize(int pixels)`` / ``int pointSize() const``", "POINTS の大きさ (正方形、1〜64 px、既定 1)"
   "``void setDepthBias(float bias)`` / ``float depthBias() const``", "以後のプリミティブの NDC 深度 (-1..1) に加える値。負で手前。共面のポリゴンの上にワイヤーフレームを描くときに使う (例: -0.002)"

ライトと背景
--------------------------------------------------------------------------------

.. csv-table::
   :header: "メンバー", "説明"

   "``void enableParallelLight(const vec3f &dir, const colorf &col)``", "平行光源。``dir`` は呼び出し時の行列で変換される"
   "``void disableParallelLight()``", ""
   "``void enableEnvironmentLight(const colorf &col)`` / ``disableEnvironmentLight()``", "環境光"
   "``void setClearColor(const colorf &)``", "背景色を設定し、背景の塗りを有効にする"
   "``void disableClear()``", "背景を塗らず、描画先の内容を残す"
   "``bool isClearEnabled() const``", ""

投影
--------------------------------------------------------------------------------

.. csv-table::
   :header: "メンバー", "説明"

   "``void setPerspectiveProjection(float fovY, float aspect, float zNear, float zFar)``", "透視投影 (fovY はラジアン)"
   "``void setOrthographicProjection(float l, float r, float b, float t, float zNear, float zFar)``", "正射影"

レンダリング
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void beginRender()``", "三角形を奥から順にソートする"
   "``void render(int16_t x, int16_t y, int16_t w, int16_t h, const Surface &dst, int16_t dstX = 0, int16_t dstY = 0)``", "画面領域 (x, y, w, h) を ``dst`` の (dstX, dstY) に描く。画面と ``dst`` の両方でクリップされる。``dst`` は RGB565BE、RGB565、RGB444 のいずれか"
   "``void render(int ctx, int16_t x, int16_t y, int16_t w, int16_t h, const Surface &dst, int16_t dstX = 0, int16_t dstY = 0)``", "レンダリングコンテキスト ``ctx`` (0〜``Config::renderContexts`` - 1) で描く。コンテキストが異なる呼び出しは同時に実行できる (コアごとに別の帯を描くなど)。同じコンテキストの呼び出しを重ねてはいけない"
   "``void endRender()``", "レンダリングを終える"
   "``Stats getStats() const``", "統計 (``endRender()`` 後に呼ぶとそのフレームの値)"

形状と静的シーン
================================================================================

``putCube()`` などの形状関数は :doc:`shapes`、``putMesh()`` / ``putNode()`` / ``putScene()`` は :doc:`scene` を参照してください。

使用例: 帯状レンダリングと統計
================================================================================

.. code-block:: cpp

   g3d.beginRender();
   for (int y = 0; y < SCREEN_H; y += BAND_H) {
     g3d.render(0, y, SCREEN_W, BAND_H, band);
     lcd.writeAsync(0, y, band);
   }
   g3d.endRender();

使用例: デュアルコア
================================================================================

``Config::renderContexts = 2`` にすると、2 つのコアで画面の上下半分を同時に描けます。
シーンの構築と ``beginRender()`` は片方のコアで行い、両方の ``render()`` が終わってから ``endRender()`` を呼びます。

.. code-block:: cpp

   g3::Config cfg = g3::defaultConfig(W, H, arena, sizeof(arena));
   cfg.renderContexts = 2;
   g3d.init(cfg);

   // core 1
   void core1Main() {
     for (;;) {
       multicore_fifo_pop_blocking();                 // フレームの準備ができた
       g3d.render(1, 0, H / 2, W, H / 2, fb, 0, H / 2);
       multicore_fifo_push_blocking(1);
     }
   }

   // core 0
   buildScene();
   g3d.beginRender();
   multicore_fifo_push_blocking(1);
   g3d.render(0, 0, 0, W, H / 2, fb);
   multicore_fifo_pop_blocking();
   g3d.endRender();

コンテキストを 1 つ増やすごとに、線分プール 1 つ分、画面 1 行あたり 4 バイト、プリミティブ 1 個あたり 2 バイトを使います。

統計の確認
--------------------------------------------------------------------------------

.. code-block:: cpp

   g3::Stats st = g3d.getStats();
   if (st.triDropped || st.spanDropped) {
     // アリーナが足りない: 大きくするか、シーンを簡略化する
   }
