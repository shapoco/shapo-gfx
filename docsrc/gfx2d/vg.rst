ベクタグラフィックス (vg)
################################################################################

ヘッダ: ``shapoco/gfx2d/vg.hpp`` (``graphics2d.hpp`` が include します)

``shapoco::gfx2d::vg`` は、直線とベジェ曲線からなるパスを、ブラシ (単色またはグラデーション) で塗り、
太さ・端点・角の形を指定してストロークし、それらをピクチャ (図形の並び) として扱うためのデータ型です。
描画は ``Graphics2D`` の ``fillPath()``、``strokePath()``、``drawPicture()`` が行います。
データは ``static const`` の構造体でフラッシュに置け、SVG から :doc:`../tools/svg2cpp` で生成するか、
``PathBuilder`` で組み立てます。名前は SVG に限らずベクタ形式一般のものです。

.. code-block:: cpp

   #include "shapoco/gfx2d/gfx2d.hpp"
   #include "logo.hpp"                          // svg2cpp の出力 (namespace logo)

   namespace g2 = shapoco::gfx2d;
   namespace vg = shapoco::gfx2d::vg;

   void frame(g2::Graphics2D &g) {
     g.setTransform(g2::affine2f::translation(20, 20).scale(2));
     g.drawPicture(logo::picture);              // 2 倍に拡大しても曲線は滑らか

     // 手で組み立てる場合
     uint8_t ops[16]; float coords[64];
     vg::PathBuilder pb(ops, 16, coords, 64);
     pb.roundRect(0, 0, 100, 40, 8, 8);
     g.resetTransform();
     g.setFillColor(0xFF4080FF);
     g.setStrokeColor(g2::Colors::WHITE);
     g.setStrokeStyle(vg::strokeStyle(2, vg::LineCap::ROUND, vg::LineJoin::ROUND));
     g.drawPath(pb.path());                     // 塗ってからストローク
   }

座標は ``affine2f`` と同じ連続量で、矩形 ``(x, y)-(x + w, y + h)`` の辺をなぞるパスは
``Rect{x, y, w, h}`` のピクセルを塗ります。曲線は描画時に、そのときの変換行列に合わせた細かさで
折れ線にするので、拡大しても角張りません。

データ構造
================================================================================

.. code-block:: cpp

   enum class PathOp : uint8_t { MOVE, LINE, QUAD, CUBIC, CLOSE };   // 座標数 2, 2, 4, 6, 0
   enum class FillRule : uint8_t { NONZERO, EVEN_ODD };

   struct Path {
     const uint8_t *ops;        // PathOp の列
     const float *coords;       // 各 op の座標 (pathOpCoords(op) 個ずつ)
     uint16_t opCount, coordCount;
     FillRule rule;
     uint8_t pad[3];
     RectF bounds;              // 制御点を含む点の範囲。空なら描画時に計算
   };

   enum class GradientKind : uint8_t { LINEAR, RADIAL };
   enum class Spread : uint8_t { PAD, REFLECT, REPEAT };   // 端の外側: 端の色 / 鏡像 / 繰り返し
   struct GradientStop { float offset; Color color; };     // offset は 0..1 の昇順
   struct Gradient {
     GradientKind kind; Spread spread; uint8_t stopCount, pad;
     const GradientStop *stops;
     affine2f toGradient;       // 描画座標 → グラデーション座標 (後述)
   };
   struct Brush { Color color; const Gradient *gradient; };   // gradient が nullptr なら単色

   enum class LineCap : uint8_t { BUTT, ROUND, SQUARE };
   enum class LineJoin : uint8_t { MITER, ROUND, BEVEL };
   struct StrokeStyle { float width; LineCap cap; LineJoin join; uint8_t pad[2]; float miterLimit; };

   enum class ShapeKind : uint8_t { PATH, IMAGE, TEXT };
   enum ShapeFlags : uint8_t { SHAPE_FILL_CURRENT_COLOR = 1, SHAPE_STROKE_CURRENT_COLOR = 2 };
   struct Text { const char *text; const GFXfont *font; float x, y; };   // (x, y) はベースライン
   struct Shape {
     ShapeKind kind; uint8_t flags, pad[2];
     const void *data;          // kind に応じて const Path * / const Texture * / const Text *
     Brush fill, stroke;        // stroke は PATH のみ
     StrokeStyle strokeStyle;
     affine2f transform;        // 図形の座標系 → ピクチャの座標系
     const RectF *clip;         // ピクチャ座標系のクリップ矩形。nullptr: なし
   };
   struct Picture { const Shape *shapes; uint16_t shapeCount, features; RectF bounds; };

- **パス**: 先頭以外の op は現在点から続きます。``CLOSE`` の直後 (または先頭) の ``LINE`` / ``QUAD`` /
  ``CUBIC`` は最後の ``MOVE`` の点から始まります。塗りつぶしは全てのサブパスを閉じ、ストロークは
  ``CLOSE`` で終わるサブパスだけを閉じます。重なった部分は ``NONZERO`` (SVG の既定。巻き数を数える) か
  ``EVEN_ODD`` (交互) で決まります。
- **ブラシ**: 単色か、グラデーション (``color`` の α がその不透明度になり、RGB は無視) です。
  グラデーションは固有の座標系を持ち、``LINEAR`` は x 軸の 0 から 1 へ、``RADIAL`` は原点から単位円へ
  変化します。``toGradient`` が描画座標 (``Graphics2D`` の変換行列を通る前) をその座標系に写すので、
  2 点間の線形、楕円形や傾いた放射も同じ仕組みで表せます。``linearGradient(p0, p1, stops, n, spread)``
  と ``radialGradient(center, radius, stops, n, spread)`` が一般的なものを作ります。
- **ストローク**: ``width`` はパスを中心にした太さで、描画座標での値なので変換行列で拡大されます
  (非等方でも正しく)。端点は ``BUTT`` / ``ROUND`` / ``SQUARE``、角は ``MITER`` / ``ROUND`` / ``BEVEL`` で、
  ``miterLimit`` は SVG と同じ意味です (角から ``miterLimit × width / 2`` より遠いマイターは BEVEL になる)。
  長さ 0 のサブパス (同じ点への ``M L``、または ``M Z``) は ``ROUND`` で点、``SQUARE`` で正方形を描き、
  ``MOVE`` だけのサブパスは何も描きません (SVG と同じ)。破線は対応していません
  (svg2cpp は静的な破線をサブパスに分割します)。
- **ピクチャ**: 図形を順に描きます。``SHAPE_FILL_CURRENT_COLOR`` / ``SHAPE_STROKE_CURRENT_COLOR`` の
  図形はブラシの色の RGB を ``drawPicture()`` に渡した色 (SVG の ``currentColor``、rig ではスロットの色)
  に置き換えます (α とグラデーションはそのまま)。``SHAPE_STROKE_CURRENT_WIDTH`` の図形はストロークの太さを
  ``drawPicture()`` に渡した太さ (rig ではスロットのストローク幅。アニメーションで変わる) に置き換えます。``clip`` は ``Graphics2D`` のクリップ矩形として適用するので、
  回転していると外接矩形になります。``IMAGE`` は図形座標系の ``(0, 0)-(width, height)`` にテクスチャを
  置き (変換行列を通した ``drawImage()``、最近傍サンプリング)、``fill.color`` の α を不透明度にします。
  ``TEXT`` はビットマップフォント ``font`` (nullptr ならコンテキストのフォント) で ``text`` を
  ``fill.color`` で描きます (ベースラインが ``(x, y)``)。
- ``FORMAT_VERSION`` と ``Picture::features`` は rig と同じ考え方の予約で、生成ヘッダは位置指定で初期化し、
  メンバは末尾にだけ追加します。``SUPPORTED_FEATURES`` にないビットが立ったピクチャは描きません。

PathBuilder
================================================================================

利用者が用意した配列に ``Path`` を組み立てます。容量を超えた op は捨てられ、``overflowed()`` が true になります。

.. csv-table::
   :header: "関数", "説明"

   "``PathBuilder(ops, opCapacity, coords, coordCapacity)``", "書き込み先の配列と容量"
   "``moveTo`` / ``lineTo`` / ``quadTo`` / ``cubicTo`` / ``close``", "op を 1 つ追加 (連鎖できる)"
   "``rect(x, y, w, h)`` / ``roundRect(x, y, w, h, rx, ry)``", "矩形、丸角矩形 (角は 3 次ベジェ)"
   "``ellipse(cx, cy, rx, ry)`` / ``circle(cx, cy, r)``", "楕円 (4 本の 3 次ベジェ)、円"
   "``arc(cx, cy, rx, ry, startAngle, endAngle)``", "楕円の円弧 (角度は ``drawArc()`` と同じパラメトリック、時計回り)。現在点から始点へ直線 (空のパスなら moveTo) のあと 1/4 周以下の 3 次ベジェ"
   "``polyline(points, n, closed)``", "折れ線 (閉じると多角形)"
   "``Path path(FillRule = NONZERO) const``", "ここまでの ``Path`` (``bounds`` を計算済み)"

``pathBounds(path)`` は制御点を含む点の範囲を返します。

Graphics2D のベクタ描画
================================================================================

ブラシ、ストロークのスタイル、アンチエイリアスのフラグはステートの一部で、``pushState()`` /
``popState()`` で保存・復元されます。

.. csv-table::
   :header: "メンバー", "説明"

   "``void setAntialias(bool)`` / ``bool antialias() const``", "アンチエイリアス (既定 false)。ベクタ描画と面の塗りつぶし (後述) に効く"
   "``void setFillBrush(const vg::Brush &)`` / ``setFillColor(Color)`` / ``fillBrush()``", "塗りのブラシ (既定は不透明の白)"
   "``void setStrokeBrush(const vg::Brush &)`` / ``setStrokeColor(Color)`` / ``strokeBrush()``", "ストロークのブラシ"
   "``void setStrokeStyle(const vg::StrokeStyle &)`` / ``setStrokeWidth(float)`` / ``strokeStyle()``", "ストロークのスタイル (既定は太さ 1、BUTT、MITER、4)"
   "``void fillPath(const vg::Path &)``", "塗りのブラシで塗る"
   "``void strokePath(const vg::Path &)``", "ストロークのブラシとスタイルでなぞる"
   "``void drawPath(const vg::Path &)``", "塗ってからなぞる"
   "``void strokePolyline(const vec2f *, int n, bool closed = false)``", "折れ線を直線のパスとしてなぞる (64 点を超えると分割)"
   "``void drawPicture(const vg::Picture &, Color currentColor = BLACK, float currentStrokeWidth = 1)``", "ピクチャの図形をそれぞれのブラシで描く。終わるとステートは元のまま"

いずれも変換行列、クリップ矩形、ブレンドモード、不透明度に従います。既存の ``fillRect()`` などの
``Color`` 引数の関数はブラシを使いませんが、アンチエイリアスが ON のときは面の塗りつぶし
(``fillRect()``、``fillEllipse()`` と円、``fillRoundRect()``、``fillPolygon()`` と三角形) が同じ形のパスとして
このラスタライザで描かれ、アンチエイリアスされます (平行移動だけの整数矩形は丸ごとピクセルなので対象外。
多角形の頂点はピクセルの中心、64 頂点まで、even-odd)。線と輪郭も対象です: ``drawLine()`` / ``drawPolyline()`` /
``drawPolygon()`` は点を先に変換行列で描画先へ写してから (拡大しても 1 ピクセル幅のまま)、Wu のアルゴリズムの
細線として描きます (主軸に沿って 1 列ごとに線がまたぐ 2 ピクセルを位置で重み付けして塗る。1 ピクセル数命令で、
行ごとの処理はなし。頂点では次の線分が最初の列のピクセルを、前の線分が置いた重みを超えるぶんだけ塗るので、
頂点が二重に塗られることも薄くなることもありません)。軸に平行な線は AA なしと同じピクセルです。``drawEllipse()`` / ``drawCircle()`` / ``drawArc()`` / ``drawRoundRect()`` は図形を半ピクセル
内側に寄せた輪郭を同じ細線で描き (塗りの内側に収まる)、``fillSector()`` は扇形のパス (円弧は
``PathBuilder::arc()`` の 3 次ベジェ)、``drawRect()`` は外側と内側の矩形の even-odd パスです
(平行移動だけの整数の枠はそのまま)。
変換行列 (拡大・回転) のかかった文字とビットマップは、ピクセルごとにマスクを 2×2 の 4 点でサンプリングして
前景を点灯ビット数 / 4 の α で、背景を消灯ビット数 / 4 の α でブレンドします (5 段階。整数倍では 4 点が
同じ画素に落ちるので変化なし。無変換の文字は元から正確で変わりません)。変換行列のかかった (または多角形で
切り抜いた) 画像は、輪郭の内側と外側 0.71 ピクセル以内に中心があるピクセルを 1 つずつ、中心の下の点の周りの
4 テクセルを乗算済み α でバイリニア補間して描きます (透明なテクセルやキーカラーのテクセルは重み 0 で色が
にじまない。4 テクセルは点が同じセルにある間は再読み込みしない。全て不透明なら乗算済みの計算を省く)。
輪郭沿いのピクセルは、辺から近似した被覆率 (辺ごとに「中心の符号付き距離 + 0.5」を 0..1 に収めたものの積。
辺に沿っては正確、角では積の近似) を α に掛けます。ラスタライザもバッファも使いません。
実装は ``src/gfx2d/images.cpp`` の ``ImageAA`` (``row<F>()`` がサンプリング、``drawImageAA()`` が入口) です。
等倍の転送はそのままです。バイリニアはホストで 1 ピクセル約 10 ns、最近傍転送の 5〜7 倍のコストです。

描画の仕組み
--------------------------------------------------------------------------------

- 曲線は、変換後の制御多角形の長さ ``L`` ピクセルに対して ``ceil(sqrt(1.5 L))`` 本 (最大 64) の線分に
  分けます (誤差はおよそ 1/10 ピクセル。密度は ``vg.cpp`` の ``segmentsFor()`` の係数で決まります)。辺はアリーナのスクラッチメモリ (1 辺 16 + 2 バイト、ストロークは
  線分 1 本あたり約 7 辺) に集め、``shapes.cpp`` の多角形と同じ走査 (1/16 ピクセルの列、中心が内側にある
  ピクセルを塗る) で、行ごとに交点を並べて塗り規則で結びます (1 行の交点は 64 個まで)。
- ストロークは線分ごとの四角形、角ごとの接合多角形、端点ごとのキャップを、全て同じ向きのループにして
  nonzero で一度に塗るので、半透明でも重なりが濃くなりません。丸い角と端点は半径に応じて 8 / 16 / 32 角形で、
  実行時に三角関数は使いません。
- 辺がメモリに収まらないときは、塗りの曲線をそのぶん粗く分割します (分割数は長さの平方根に比例するので、
  不足分の 2 乗で粗くなる。ストロークのループは凸で途中で切っても正しいので粗くしません)。それでも収まらないループは途中で切り、両方を最初の点への弦で閉じます。
  凸なループ (ストロークのループは全て凸) ならこれで正確で、凹な塗りでは弦が見えることがあります。
  パスは部分ごとに順に描かれるので、半透明では部分の継ぎ目が見えることがあります。
  アリーナがなくてもスタック上の 24 辺のバッファで同じように描きます。
- **アンチエイリアス** (``setAntialias()``、既定 OFF、``SHAPOGFX2D_ANTIALIAS``): 1 ピクセル行を 4 本のサブ行で走査し、
  各サブ行で内側にある 1/16 列の数を足してピクセルの被覆率 (0..64) にします。これはそのままブレンドの
  重み (alpha64) で、半分覆われたピクセルは色の α × 不透明度の半分でブレンドされます。
  被覆率は 1 行ぶんの差分配列 (パスの幅 1 ピクセルあたり 2 バイト。スクラッチメモリか、128 ピクセルまでは
  スタック) に集め、単色なら等しい被覆率の連続をまとめて、グラデーションならピクセルごとに塗ります。
  ``NONE`` のブレンドモード (上書き) では効きません。面の塗りつぶしは上記のとおり同じ経路で
  アンチエイリアスされ、直線、輪郭、画像、文字はされません。
- グラデーションは呼び出しごとに 64 色の表を作り、ピクセルの位置を Q16 の整数で進めます (32 ピクセルごとに
  正確な値から始めるのでずれは溜まりません)。放射の距離は Q12 の成分の ``isqrt32`` で求めます (半径の 8 倍で
  飽和)。ピクセルごとの浮動小数点演算はありません。

コンパイル時オプション
================================================================================

``SHAPOGFX2D_ANTIALIAS=0`` (既定 1) でアンチエイリアスのコードを除去します。``setAntialias()`` に関係なく
多角形と同じ描き方になります (``src/gfx2d`` のみ。公開型は変わりません)。

コード量と性能の目安
================================================================================

``src/gfx2d/vg.cpp`` のコードは Cortex-M33 で 26.0 KB、Cortex-M0+ で 31.4 KB です (``-O2``。うち約 3 KB が
線・輪郭・扇形・枠のアンチエイリアス。塗りつぶし・線がアンチエイリアス用に参照するので、``Graphics2D`` を使う
プログラムにはリンクされます)。文字とビットマップのサンプリングと画像のバイリニア補間は ``images.cpp`` に 11.4 KB
(ソース形式ごとのサンプラ)、各描画関数の分岐は ``graphics2d.cpp`` / ``shapes.cpp`` に合わせて約 1.5 KB です。
AA を ON にした demorig のフレームは、ホストでの計測で OFF の約 7 倍のコストで、ほぼ全てがキャラクタの
バイリニア補間です (塗りと細線は約 2 倍)。OFF のときは呼び出しごとのフラグ判定だけです。ピクセルあたりのコストは多角形と同じで、アンチエイリアスではサブ行 4 本ぶんの
辺の歩みと、輪郭のピクセルのブレンドが加わります。ストロークは線分 1 本につき四角形 1 つと接合の多角形
(3〜4 頂点、丸なら 8〜32) ぶんの辺です。

複雑なイラストをフレームごとに描くのは組み込みには向きません。静止したピクチャは一度 ``Surface`` に
描いておき、``drawImage()`` で使うのが安上がりです。
