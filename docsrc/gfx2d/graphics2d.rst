Graphics2D
################################################################################

ヘッダ: ``shapoco/gfx2d/graphics2d.hpp``

概要
================================================================================

``Graphics2D`` は ``Surface`` への描画コンテキストです。

- 描画先はコンストラクタまたは ``setTarget()`` で与えます。``Surface`` 構造体のコピーを保持するだけで、
  ピクセルバッファの寿命は利用者が管理します。
- 色は全て ``Color`` (ARGB8888) です。α が 255 なら上書き、0 なら何も描かず、その間ならブレンドします。
- 全ての描画はクリップ矩形 (既定は描画先全体) で切り取られます。
- メモリ確保はしません。

.. code-block:: cpp

   g2::Graphics2D g(surface);
   g.clear(g2::makeColor(20, 24, 40));
   g.fillRect(10, 10, 100, 50, g2::Colors::RED);
   g.drawLine(0, 0, 319, 239, g2::makeColor(255, 255, 255, 128));

描画先と状態
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``Graphics2D()`` / ``explicit Graphics2D(const Surface &)``", "コンストラクタ"
   "``void setTarget(const Surface &)``", "描画先を設定し、クリップ矩形をリセットする。無効化されたフォーマットと、幅・高さが ``SHAPOGFX_COORD_MAX`` を超える Surface は描画先として拒否される"
   "``const Surface &target() const`` / ``bool hasTarget() const``", "描画先の取得 / 有無"
   "``PixelFormat format() const`` / ``Rect bounds() const``", "描画先のフォーマット / 全体矩形"
   "``void setClipRect(const Rect &)`` / ``setClipRect(x, y, w, h)``", "クリップ矩形 (描画先と交差した範囲になる)"
   "``void resetClipRect()`` / ``const Rect &clipRect() const``", "クリップ矩形のリセット / 取得"
   "``const GraphicsState2D &state() const`` / ``void setState(const GraphicsState2D &)``", "クリップ矩形と文字設定をまとめて保存・復元する"

矩形は半開区間 ``[x, x + w) x [y, y + h)`` で、負のサイズは正規化されます (:doc:`../gfx3d/math` の ``Rect`` を参照)。

ピクセルと矩形
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void clear(Color)``", "クリップ矩形全体を塗る"
   "``void setPixel(int x, int y, Color)``", "1 ピクセル描く"
   "``Color getPixel(int x, int y) const``", "1 ピクセル読む (描画先の外は ``TRANSPARENT``)"
   "``void fillRect(const Rect &, Color)`` / ``fillRect(x, y, w, h, Color)``", "塗りつぶし矩形"
   "``void drawRect(const Rect &, Color, int thickness = 1)`` / ``drawRect(x, y, w, h, Color, thickness)``", "矩形の輪郭。矩形の内側に ``thickness`` ピクセル幅で描く"
   "``void fillRoundRect(const Rect &, int radius, Color)`` / ``fillRoundRect(x, y, w, h, radius, Color)``", "丸角矩形の塗り"
   "``void drawRoundRect(const Rect &, int radius, Color)`` / ``drawRoundRect(x, y, w, h, radius, Color)``", "丸角矩形の輪郭 (1 px)"
   "``void drawHLine(x, y, w, Color)`` / ``drawVLine(x, y, h, Color)``", "水平線 / 垂直線"

楕円と円
================================================================================

楕円は与えた矩形に内接します。輪郭は塗りと整合する 1 ピクセル幅の閉じた線になります。

.. csv-table::
   :header: "メンバー", "説明"

   "``void fillEllipse(const Rect &, Color)`` / ``fillEllipse(x, y, w, h, Color)``", "楕円の塗り"
   "``void drawEllipse(const Rect &, Color)`` / ``drawEllipse(x, y, w, h, Color)``", "楕円の輪郭"
   "``void fillCircle(int cx, int cy, int radius, Color)``", "円の塗り (直径 ``2 * radius + 1``)"
   "``void drawCircle(int cx, int cy, int radius, Color)``", "円の輪郭"

円弧と扇形
================================================================================

楕円 (矩形に内接) のうち、角度 ``startAngle`` から ``endAngle`` の範囲にある部分を描きます。
``drawArc`` は ``drawEllipse`` の、``fillSector`` は ``fillEllipse`` のピクセルのうち範囲内のものだけを描くので、
楕円と同じ形になります。

.. csv-table::
   :header: "メンバー", "説明"

   "``void drawArc(const Rect &, float startAngle, float endAngle, Color)`` / ``drawArc(x, y, w, h, ...)``", "楕円弧"
   "``void fillSector(const Rect &, float startAngle, float endAngle, Color)`` / ``fillSector(x, y, w, h, ...)``", "塗りつぶした扇形 (パイ)"
   "``void drawCircleArc(int cx, int cy, int radius, float startAngle, float endAngle, Color)``", "円弧"
   "``void fillCircleSector(int cx, int cy, int radius, float startAngle, float endAngle, Color)``", "円の扇形"

- 角度はラジアンで、+x 軸から画面上で時計回りに測ります。
- 角度は **媒介変数の角度** です。楕円を円を引き伸ばしたものとみなし、その円の上で角度を取ります
  (角度 ``t`` は ``(rx cos t, ry sin t)`` の方向)。このため 45° は外接矩形の角を指し、同じ角度の扇形は同じ面積になります
  (楕円の円グラフでも比率が保たれる)。
- ``endAngle`` は ``startAngle`` の後ろへ 2π を法として取ります。``(0, -π/2)`` は 3/4 周です。
  ``endAngle - startAngle >= 2π`` なら楕円全体、``endAngle == startAngle`` なら何も描きません。
- 角度を共有する扇形同士は重ならず、隙間もできません (境界上のピクセルはどちらか一方だけに入る)。
  円グラフを半透明で描いても継ぎ目が二重になりません。

.. code-block:: cpp

   // 円グラフ
   float a = 0;
   for (int i = 0; i < n; i++) {
     const float sweep = values[i] * (2 * PI / total);
     g.fillSector(40, 40, 120, 80, a, a + sweep, colors[i]);
     a += sweep;
   }

線と多角形
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void drawLine(int x0, int y0, int x1, int y1, Color)`` / ``drawLine(const vec2i &, const vec2i &, Color)``", "線分。両端点を含む"
   "``void drawPolyline(const vec2i *points, int count, Color)``", "折れ線"
   "``void drawPolygon(const vec2i *points, int count, Color)``", "閉じた多角形の輪郭"
   "``void fillPolygon(const vec2i *points, int count, Color)``", "多角形の塗り (偶奇規則、1 ラインあたり最大 16 交点)"
   "``void fillTriangle(x0, y0, x1, y1, x2, y2, Color)`` / ``drawTriangle(...)``", "三角形の塗り / 輪郭"

線分は長軸方向に 16.16 固定小数で歩き、クリップ範囲外の部分は歩く前に切り捨てます。同じ行に並ぶピクセルはスパンとしてまとめて塗られます。

画像
================================================================================

.. code-block:: cpp

   void drawImage(const Texture &img, int dx, int dy,
                  BlendMode mode = BlendMode::ALPHA, int opacity = 255);
   void drawImage(const Texture &img, int dx, int dy, const Rect &src,
                  BlendMode mode = BlendMode::ALPHA, int opacity = 255);

任意フォーマットの画像を (dx, dy) に描きます。描画先とフォーマットが異なる場合は変換されます。

.. csv-table::
   :header: "``mode``", "動作"

   "``BlendMode::NONE``", "コピー。ARGB4444 の α は、描画先が ARGB4444 ならコピーされ、それ以外では無視される"
   "``BlendMode::ALPHA``", "ソースの α (ARGB4444 のみ) でブレンド。α を持たないフォーマットは ``opacity`` が 255 ならコピー"
   "``BlendMode::ADD``", "加算合成"

``opacity`` (0..255) は ``ALPHA`` と ``ADD`` で α に乗算されます。
``src`` で画像の一部だけを描けます。同じ 16bit フォーマット同士のコピーは ``memcpy`` になります。

拡大縮小
--------------------------------------------------------------------------------

.. code-block:: cpp

   void drawImage(const Texture &img, const Rect &dst, const Rect &src,
                  BlendMode mode = BlendMode::ALPHA, int opacity = 255);
   void drawImage(const Texture &img, const Rect &dst,
                  BlendMode mode = BlendMode::ALPHA, int opacity = 255);  // 画像全体
   void drawImage(const Texture &img, int dx, int dy, int dw, int dh,
                  int sx, int sy, int sw, int sh,
                  BlendMode mode = BlendMode::ALPHA, int opacity = 255);

画像の ``src`` の範囲を ``dst`` に引き伸ばして描きます (最近傍)。描画先のピクセル ``t`` (``dw`` ピクセル中) は、
その中心の下にあるソースピクセル ``floor((2t + 1) sw / 2dw)`` を表示します。

- ``dst`` の幅・高さが負なら、その方向に鏡像反転します (矩形は正規化され、ソースを逆から数える)。
  ``src`` の負のサイズは正規化されるだけです。
- ``src`` のうち画像の外の部分は描かれません (拡大率は ``src`` のまま)。
- 幅・高さは 32767 まで (それを超えると何も描かない)。写像は整数のみで正確に計算されます (浮動小数点を使わない)。
- 等倍で反転なしなら通常の ``drawImage()`` になります。
- ``mode`` と ``opacity`` は通常の ``drawImage()`` と同じです。

アフィン変換
--------------------------------------------------------------------------------

.. code-block:: cpp

   void drawImage(const Texture &img, const affine2f &m, const Rect &src,
                  BlendMode mode = BlendMode::ALPHA, int opacity = 255);
   void drawImage(const Texture &img, const affine2f &m,
                  BlendMode mode = BlendMode::ALPHA, int opacity = 255);  // 画像全体

``m`` (:doc:`../gfx3d/math` の ``affine2f``) で画像の座標 (``src`` の左上が原点) を描画先の座標へ写し、
各ピクセルの中心の下にある画像のピクセルを描きます (最近傍)。

.. code-block:: cpp

   // スプライトを (x, y) を中心に angle 回転・1.5 倍して描く
   g.drawImage(sprite, g2::affine2f::placement(x, y, angle, 1.5f, 1.5f,
                                              sprite.width * 0.5f, sprite.height * 0.5f));

- ``src`` のうち画像の内側だけが描かれ、その外は決して読みません。
- 幅・高さ 16384 ピクセルを超える画像と、4096 分の 1 より強く縮小する変換は何も描きません
  (固定小数点を 32 ビットに収めるための制限)。
- 回転もせん断もなく、角が整数ピクセルに乗る変換は、正確な拡大縮小 (または等倍) の ``drawImage()`` に回されます。
- ``mode`` と ``opacity`` は通常の ``drawImage()`` と同じです。
- RP2040 / RP2350 では ``SHAPOGFX2D_RP2_INTERP`` (既定で有効) により、ストライドが 2 の冪の 16 ビット画像
  (幅 16 / 32 / 64 などの ARGB4444 / RGB565 スプライト) のピクセル参照を SIO interpolator ``interp0`` で行います。
  呼び出し中は ``interp0`` を保存・復元するので、その間に割り込みハンドラで ``interp0`` を使わないでください。

ビットマップ
--------------------------------------------------------------------------------

.. code-block:: cpp

   void drawBitmap(const Texture &bitmap, int dx, int dy, Color fg,
                   Color bg = Colors::TRANSPARENT);
   void drawBitmap(const Texture &bitmap, int dx, int dy, const Rect &src, Color fg,
                   Color bg = Colors::TRANSPARENT);

GRAY1 の画像を 2 色のマスクとして描きます。1 のビットを ``fg``、0 のビットを ``bg`` で塗ります。
``bg`` が ``TRANSPARENT`` なら 0 のビットは触りません。

文字
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void setFont(const GFXfont *font, int scale = 1)``", "フォントと整数倍率を設定する (アセントと行ボックス高さを計算する)"
   "``void setTextScale(int scale)``", "倍率のみ変更"
   "``void setTextColor(Color fg, Color bg = Colors::TRANSPARENT)``", "前景色と背景色。背景が ``TRANSPARENT`` 以外ならグリフのボックスを背景色で塗る"
   "``void setCursor(int x, int y)`` / ``vec2i cursor() const``", "カーソル (次のグリフの行ボックス左上)"
   "``int drawChar(int x, int y, int code)``", "1 文字を行ボックス左上 (x, y) に描き、進み幅 (倍率込み) を返す"
   "``void drawString(const char *)``", "カーソル位置から描き、カーソルを進める。``'\\n'`` で改行"
   "``void drawString(int x, int y, const char *)``", "``setCursor()`` してから描く"
   "``int measureText(const char *) const``", "最も幅の広い行の幅 (倍率込み)"
   "``int charAdvance(int code) const``", "1 文字の進み幅"
   "``int textHeight() const``", "行ボックスの高さ (倍率込み)"
   "``int lineAdvance() const``", "行送り (フォントの ``yAdvance`` x 倍率)"
   "``const TextState &textState() const``", "文字設定の取得"

カーソルは **行ボックスの左上** です。``setFont()`` は全グリフからベースラインより上の最大高さ (アセント) と
ボックス高さを求め、グリフはベースラインを基準に配置されます。この規約により、フォントを切り替えても
同じ ``y`` を渡せば上端が揃います。

右寄せの例:

.. code-block:: cpp

   const char *s = "12.3 s";
   g.setFont(&ShapoSansP_s12c09a01w02);
   g.drawString(box.right() - 8 - g.measureText(s), box.y + 4, s);

GraphicsState2D / TextState
================================================================================

.. code-block:: cpp

   struct TextState {
     const GFXfont *font;
     int scale;
     int ascent, lineHeight;     // 倍率なしのピクセル
     Color color, background;
     int cursorX, cursorY, lineStartX;
   };

   struct GraphicsState2D {
     Rect clip;
     TextState text;
   };

``state()`` / ``setState()`` で一時的にクリップやフォントを変えて元に戻すことができます。

実装上の注意
================================================================================

- 描画関数はフォーマットの分岐を呼び出しごと (または行ごと) に 1 回行い、ピクセルループはフォーマットごとに
  テンプレートで特殊化されています。
- 楕円と丸角矩形は行ごとの水平範囲 (1 行あたり平方根 1 回) で表し、輪郭は「隣接する両方の行に覆われない部分 + 端点」として
  描かれます。
- フォーマットが異なる ``drawImage()`` は 64 ピクセル単位でスタック上のバッファを介して ``Color`` に変換します。
- 拡大縮小の ``drawImage()`` は軸ごとの整数 DDA で進みます。縮小は描画先 1 ピクセルごとにソースを進め、
  拡大はソースのピクセルごとに描画先の連続 (``q`` または ``q + 1`` ピクセル) を求めて、コピーと ARGB4444 スプライトでは
  ``fill()`` でまとめて書きます。同じソース行が続く行は、16 ビットへのコピーなら前の行を ``memcpy`` します。
- アフィン変換の ``drawImage()`` は逆変換を float で 1 度求め、行ごとに 16.16 固定小数点で画像内に収まる範囲を正確に
  切り出してから、ピクセルごとに ``u += du``, ``v += dv`` と歩きます (ピクセルごとの範囲検査なし)。
  FPU のないコアでは行ごとに数回のソフトウェア浮動小数点演算が入りますが、ピクセルごとの処理は整数のみです。
- 円弧と扇形は、角度範囲の各辺を整数の方向ベクトルに 1 度丸めた半平面として表し、楕円の各行を整数除算 2 回で切り取ります。
