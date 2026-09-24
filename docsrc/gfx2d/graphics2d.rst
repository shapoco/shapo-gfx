Graphics2D
################################################################################

ヘッダ: ``shapoco/gfx2d/graphics2d.hpp``

概要
================================================================================

``Graphics2D`` は ``Surface`` への描画コンテキストです。

- 描画先はコンストラクタまたは ``setTarget()`` で与えます。``Surface`` 構造体のコピーを保持するだけで、
  ピクセルバッファの寿命は利用者が管理します。
- 色は全て ``Color`` (ARGB8888) です。色の置き方はステートのブレンドモードと不透明度 (``setBlend()``) で決まり、
  既定 (``ALPHA``, 255) では α が 255 なら上書き、0 なら何も描かず、その間ならブレンドします。
- 座標はステートの変換行列 (``setTransform()`` など) を通ってから、クリップ矩形 (既定は描画先全体) で切り取られます。
- メモリ確保はしません。``pushState()`` と一部の変形された図形は ``init()`` で渡したアリーナを使います。
  アリーナがなくても ``pushState()`` 以外は全て描けます。

.. code-block:: cpp

   static uint8_t arena[4096];

   g2::Graphics2D g(surface);
   g.init(arena, sizeof(arena));      // ステートスタックとスクラッチメモリ (省略可)
   g.clear(g2::makeColor(20, 24, 40));
   g.fillRect(10, 10, 100, 50, g2::Colors::RED);
   g.drawLine(0, 0, 319, 239, g2::makeColor(255, 255, 255, 128));

   g.pushState();
   g.translate(160, 120);
   g.rotate(0.3f);
   g.fillRect(-40, -20, 80, 40, g2::Colors::YELLOW);   // 回転した矩形
   g.popState();

メモリとステート
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``bool init(const Config &, void *arena, size_t arenaSize)`` / ``init(arena, arenaSize)``", "アリーナを渡す。ステートスタック (``SHAPOGFX2D_STACK_DEPTH`` 段) を置き、残りをスクラッチメモリにする。スタックより小さければ ``false``。``Config`` は今のところ空"
   "``void deinit()`` / ``bool isInitialized() const``", "アリーナの使用をやめる / アリーナの有無"
   "``static size_t arenaBytes(size_t scratchBytes = 2048)``", "スタックとスクラッチ ``scratchBytes`` バイトに必要なアリーナのサイズ"
   "``bool pushState()``", "ステート全体 (変換行列、クリップ、ブレンド、カラーキー、フォント、文字色) をスタックに積む。満杯かアリーナがなければ ``false`` で何もしない"
   "``void popState()``", "積んだステートに戻す。テキストカーソルだけは戻さない"
   "``int stateDepth() const``", "積まれている段数"
   "``const GraphicsState2D &state() const`` / ``void setState(const GraphicsState2D &)``", "ステートをまとめて取得・設定する (アリーナ不要)"

スクラッチメモリは、12 辺を超える多角形 (1 辺 24 バイト) と、回転した丸角矩形の角を細かく分割するのに使います。
足りないときは、多角形は行ごとに全辺を頂点から計算し直す (遅いが同じ結果)、丸角矩形は角を 4 分割で近似します。

描画先とクリップ
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``Graphics2D()`` / ``explicit Graphics2D(const Surface &)``", "コンストラクタ"
   "``void setTarget(const Surface &)``", "描画先を設定し、クリップ矩形をリセットする。無効化されたフォーマットと、幅・高さが ``SHAPOGFX_COORD_MAX`` を超える Surface は描画先として拒否される"
   "``const Surface &target() const`` / ``bool hasTarget() const``", "描画先の取得 / 有無"
   "``PixelFormat format() const`` / ``Rect bounds() const``", "描画先のフォーマット / 全体矩形"
   "``void setClipRect(const Rect &)`` / ``setClipRect(x, y, w, h)``", "クリップ矩形 (描画先と交差した範囲になる)。描画先のピクセル座標で、変換行列は効かない"
   "``void resetClipRect()`` / ``Rect clipRect() const``", "クリップ矩形のリセット / 取得"

矩形は半開区間 ``[x, x + w) x [y, y + h)`` で、負のサイズは正規化されます (:doc:`../gfx3d/math` の ``Rect`` を参照)。

変換行列
================================================================================

描画関数に渡した座標を描画先のピクセル座標へ写すアフィン変換です (:doc:`../gfx3d/math` の ``affine2f``)。
パースの付いた変換はできません。

.. csv-table::
   :header: "メンバー", "説明"

   "``void setTransform(const affine2f &)`` / ``void resetTransform()``", "設定 / 単位行列に戻す"
   "``const affine2f &transform() const``", "取得"
   "``void applyTransform(const affine2f &m)``", "``transform = transform * m`` (``m`` が先に効く)"
   "``void translate(x, y)`` / ``scale(sx, sy)`` / ``scale(s)`` / ``rotate(angle)`` / ``rotate(angle, cx, cy)``", "右から掛ける (canvas と同じく、後に書いたものが先に効く)。角度はラジアン、画面上で時計回り"
   "``TransformKind transformKind() const``", "行列の種類: ``IDENTITY`` / ``TRANSLATE`` (平行移動のみ) / ``SCALE`` (拡大縮小・鏡像と平行移動) / ``AFFINE`` (回転・せん断を含む)"

行列の種類は変更時に 1 度だけ判定され、描画関数はそれでコードパスを選びます。

- ``IDENTITY`` / ``TRANSLATE`` では変換行列がない場合と同じコードで描きます (整数座標に整数のずれを足すだけ)。
  平行移動の端数はピクセルに丸められます。
- ``SCALE`` では矩形・楕円・画像は軸に平行なまま、角をピクセルに丸めた矩形として描きます。
- ``AFFINE`` では矩形は多角形、楕円は一般の楕円、画像はアフィン写像として描きます。
- π の倍数の回転で残る 1e-6 未満の sin 成分は無視します (180° 回転は ``SCALE`` 扱い)。

座標の規約:

- 座標は連続量で、ピクセル ``(x, y)`` は ``[x, x + 1) x [y, y + 1)`` を覆います。**面**
  (矩形、楕円、画像) は中心がその内側にあるピクセルを塗ります。変換後の辺を中心が
  ちょうど通る場合は右側・下側のピクセルに属します。
- **点** (線分の端点、多角形の頂点、``setPixel()``、円の中心) はピクセルを指し、その中心が変換されて、中心を含むピクセルになります。
- **線** (``drawLine()``、``drawHLine()``、楕円や丸角矩形の輪郭など) は変換後も 1 ピクセル幅です。
  ``drawRect()`` の ``thickness`` は面として扱うので、拡大すると太くなります。
- 楕円の角度は変換前の角度です。鏡像変換では向きが反転します。

ブレンドとカラーキー
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void setBlend(BlendMode mode, int opacity = 255)``", "ブレンドモードと不透明度 (0..255)"
   "``void setBlendMode(BlendMode)`` / ``void setOpacity(int)``", "片方だけ変更"
   "``BlendMode blendMode() const`` / ``int opacity() const``", "取得"
   "``void setColorKey(Color)`` / ``void clearColorKey()``", "``drawImage()`` のカラーキーの設定 / 解除"
   "``bool hasColorKey() const`` / ``Color colorKey() const``", "取得"

.. csv-table::
   :header: "``BlendMode``", "図形・文字", "画像"

   "``ALPHA`` (既定)", "色の α x 不透明度でブレンド", "ピクセルの α (ARGB4444 のみ) x 不透明度でブレンド。α のないフォーマットで不透明度 255 ならコピー"
   "``ADD``", "色を α x 不透明度で重み付けして飽和加算", "同左"
   "``NONE``", "色で上書き。描画先が ARGB4444 なら色の α も書く", "コピー。ARGB4444 同士なら α もコピー"

``clear()`` はブレンドに関係なく上書きします。

カラーキーは ``drawImage()`` にだけ効きます。画像のピクセルのうちキーの色に一致するものは描かれません。
比較は、キーを画像のフォーマットに変換した値で行います (ARGB4444 では α も比較)。

ピクセルと矩形
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void clear(Color)``", "クリップ矩形全体を上書きする (変換行列とブレンドは効かない)"
   "``void setPixel(int x, int y, Color, bool transformed = true)``", "1 ピクセル描く。``transformed`` が false なら変換行列を通さない"
   "``Color getPixel(int x, int y, bool transformed = true) const``", "1 ピクセル読む (描画先の外は ``TRANSPARENT``)"
   "``void fillRect(const Rect &, Color)`` / ``fillRect(x, y, w, h, Color)`` / ``fillRect(const RectF &, Color)``", "塗りつぶし矩形"
   "``void drawRect(const Rect &, Color, int thickness = 1)`` / ``drawRect(x, y, w, h, Color, thickness)`` / ``drawRect(const RectF &, Color, float thickness = 1)``", "矩形の輪郭。矩形の内側に ``thickness`` の幅で描く"
   "``void fillRoundRect(const Rect &, int radius, Color)`` / ``fillRoundRect(x, y, w, h, radius, Color)`` / ``fillRoundRect(const RectF &, float, Color)``", "丸角矩形の塗り"
   "``void drawRoundRect(...)``", "丸角矩形の輪郭 (1 ピクセル幅)"
   "``void drawHLine(x, y, w, Color)`` / ``drawVLine(x, y, h, Color)``", "1 ピクセル幅の水平線 / 垂直線"

楕円と円
================================================================================

楕円は与えた矩形に内接します。輪郭は塗りと整合する 1 ピクセル幅の閉じた線になります。

.. csv-table::
   :header: "メンバー", "説明"

   "``void fillEllipse(const Rect &, Color)`` / ``fillEllipse(x, y, w, h, Color)`` / ``fillEllipse(const RectF &, Color)``", "楕円の塗り"
   "``void drawEllipse(...)``", "楕円の輪郭"
   "``void fillCircle(int cx, int cy, int radius, Color)`` / ``fillCircle(const vec2f &center, float radius, Color)``", "円の塗り (直径 ``2 * radius + 1``)"
   "``void drawCircle(...)``", "円の輪郭"

円弧と扇形
================================================================================

楕円 (矩形に内接) のうち、角度 ``startAngle`` から ``endAngle`` の範囲にある部分を描きます。
``drawArc`` は ``drawEllipse`` の、``fillSector`` は ``fillEllipse`` のピクセルのうち範囲内のものだけを描くので、
楕円と同じ形になります。

.. csv-table::
   :header: "メンバー", "説明"

   "``void drawArc(const Rect &, float startAngle, float endAngle, Color)`` / ``drawArc(x, y, w, h, ...)`` / ``drawArc(const RectF &, ...)``", "楕円弧"
   "``void fillSector(...)``", "塗りつぶした扇形 (パイ)"
   "``void drawCircleArc(int cx, int cy, int radius, float startAngle, float endAngle, Color)`` / ``drawCircleArc(const vec2f &, float, ...)``", "円弧"
   "``void fillCircleSector(...)``", "円の扇形"

- 角度はラジアンで、+x 軸から画面上で時計回りに測ります。
- 角度は **媒介変数の角度** です。楕円を円を引き伸ばしたものとみなし、その円の上で角度を取ります
  (角度 ``t`` は ``(rx cos t, ry sin t)`` の方向)。このため 45° は外接矩形の角を指し、同じ角度の扇形は同じ面積になります
  (楕円の円グラフでも比率が保たれる)。変換行列がある場合も、変換前の楕円の上の角度です。
- ``endAngle`` は ``startAngle`` の後ろへ 2π を法として取ります。``(0, -π/2)`` は 3/4 周です。
  ``endAngle - startAngle >= 2π`` なら楕円全体、``endAngle == startAngle`` なら何も描きません。
- 角度を共有する扇形同士は重ならず、隙間もできません (境界上のピクセルはどちらか一方だけに入る)。
  円グラフを半透明で描いても継ぎ目が二重になりません。回転・せん断・鏡像の変換の下でも同じです。

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

   "``void drawLine(int x0, int y0, int x1, int y1, Color)`` / ``drawLine(const vec2i &, const vec2i &, Color)`` / ``drawLine(const vec2f &, const vec2f &, Color)``", "線分。両端点を含む"
   "``void drawPolyline(const vec2i *points, int count, Color)`` / ``vec2f`` 版", "折れ線"
   "``void drawPolygon(const vec2i *points, int count, Color)`` / ``vec2f`` 版", "閉じた多角形の輪郭"
   "``void fillPolygon(const vec2i *points, int count, Color)`` / ``vec2f`` 版", "多角形の塗り (偶奇規則、1 ラインあたり最大 32 交点)"
   "``void fillTriangle(x0, y0, x1, y1, x2, y2, Color)`` / ``fillTriangle(const vec2f &, const vec2f &, const vec2f &, Color)`` / ``drawTriangle(...)``", "三角形の塗り / 輪郭"

- 線分は長軸方向に 16.16 固定小数で歩き、クリップ範囲外の部分は歩く前に切り捨てます。同じ行に並ぶピクセルはスパンとしてまとめて塗られます。
- 多角形の頂点は線分の端点と同じくピクセルを指し、辺は頂点ピクセルの中心を通ります
  (変換行列があれば、頂点の中心を変換して行き着いたピクセルの中心。``drawLine()`` と同じ)。
  中心が内側にあるピクセルを塗る (辺の上なら左辺・上辺側のみ) ので、同じ頂点で ``drawPolygon()`` した輪郭から
  塗りがはみ出しません。矩形の角 ``(x, y)``-``(x + w, y + h)`` を頂点とする多角形は ``Rect{x, y, w, h}`` のピクセルを塗ります。
- 辺ごとに最初の行で 1 回除算してから整数 DDA で正確に歩くので、2 つの多角形が共有する辺は両方で同じ列になり、
  隙間も重なりもできません。

画像
================================================================================

.. code-block:: cpp

   void drawImage(const Texture &img, int dx, int dy);
   void drawImage(const Texture &img, int dx, int dy, const Rect &src);
   void drawImage(const Texture &img, const Rect &dst, const Rect &src);   // 拡大縮小
   void drawImage(const Texture &img, const Rect &dst);
   void drawImage(const Texture &img, int dx, int dy, int dw, int dh,
                  int sx, int sy, int sw, int sh);

任意フォーマットの画像を描きます。描画先とフォーマットが異なる場合は変換されます。
ブレンドモード、不透明度、カラーキーはステートのものが使われ、位置と大きさは変換行列を通ります。

- ``src`` で画像の一部だけを描けます。``src`` のうち画像の外の部分は描かれず、その場所は空きます。
- ``dst`` を与えると ``src`` をその大きさに引き伸ばします (最近傍)。描画先のピクセル ``t`` (``dw`` ピクセル中) は、
  その中心の下にあるソースピクセル ``floor((2t + 1) sw / 2dw)`` を表示します。
  ``dst`` の幅・高さが負ならその方向に鏡像反転します。幅・高さは 32767 まで (それを超えると何も描かない)。

処理の重さに応じてコードパスが分かれます。

.. csv-table::
   :header: "条件", "処理"

   "平行移動のみ、同じ 16 ビットフォーマットのコピー (``NONE``、または α のない画像の ``ALPHA`` 255)", "行ごとの ``memcpy``"
   "平行移動のみ、カラーキーなし", "フォーマットの組ごとの行ループ (変換、ARGB4444 スプライトのブレンド、同じフォーマットの不透明度付きブレンド)。それ以外 (加算など) は 64 ピクセル単位で ``Color`` を経由"
   "拡大縮小 (``dst`` 指定、または ``SCALE`` の変換行列)、またはカラーキー付き", "軸ごとの整数 DDA。拡大ではソースのピクセルごとに描画先の連続を ``fill()`` で書く (コピー、カラーキー付きコピー、ARGB4444 スプライト)"
   "回転・せん断 (``AFFINE``)", "アフィン写像。逆変換を float で 1 度求め、行ごとに 16.16 固定小数点で画像内に収まる範囲を正確に切り出してから、ピクセルごとに ``u += du``, ``v += dv`` と歩く (ピクセルごとの範囲検査なし)"

- ``SCALE`` の変換行列では、画像の角を面と同じ規則でピクセルに丸め、その矩形への拡大縮小として描きます。
  同じ変換の ``fillRect()`` と同じピクセルを覆います。
- アフィン写像では、幅・高さ 16384 ピクセルを超える画像と、4096 分の 1 より強く縮小する変換は何も描きません
  (固定小数点を 32 ビットに収めるための制限)。
- RP2040 / RP2350 では ``SHAPOGFX2D_RP2_INTERP`` (既定で有効) により、アフィン写像のうちストライドが 2 の冪の
  16 ビット画像 (幅 16 / 32 / 64 などの ARGB4444 / RGB565 スプライト) のピクセル参照を SIO interpolator ``interp0`` で行います。
  呼び出し中は ``interp0`` を保存・復元するので、その間に割り込みハンドラで ``interp0`` を使わないでください。

.. code-block:: cpp

   // スプライトを (x, y) を中心に angle 回転・1.5 倍して描く
   g.pushState();
   g.setTransform(g2::affine2f::placement(x, y, angle, 1.5f, 1.5f,
                                          sprite.width * 0.5f, sprite.height * 0.5f));
   g.drawImage(sprite, 0, 0);
   g.popState();

   // マゼンタを抜いて描く
   g.setColorKey(g2::Colors::MAGENTA);
   g.drawImage(sheet, 10, 10, g2::Rect{32, 0, 16, 16});
   g.clearColorKey();

ビットマップ
--------------------------------------------------------------------------------

.. code-block:: cpp

   void drawBitmap(const Texture &bitmap, int dx, int dy, Color fg,
                   Color bg = Colors::TRANSPARENT);
   void drawBitmap(const Texture &bitmap, int dx, int dy, const Rect &src, Color fg,
                   Color bg = Colors::TRANSPARENT);

GRAY1 の画像を 2 色のマスクとして描きます。1 のビットを ``fg``、0 のビットを ``bg`` で塗ります。
``bg`` が ``TRANSPARENT`` なら 0 のビットは触りません。変換行列とブレンドが効きます。

文字
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void setFont(const GFXfont *font)`` / ``const GFXfont *font() const``", "フォントを設定する (アセントと行ボックス高さを計算する)"
   "``void setTextColor(Color fg, Color bg = Colors::TRANSPARENT)``", "前景色と背景色。背景が ``TRANSPARENT`` 以外ならグリフのボックスを背景色で塗る"
   "``void setCursor(int x, int y)`` / ``vec2i cursor() const``", "カーソル (次のグリフの行ボックス左上)"
   "``int drawChar(int x, int y, int code)``", "1 文字を行ボックス左上 (x, y) に描き、進み幅を返す"
   "``void drawString(const char *)``", "カーソル位置から描き、カーソルを進める。``'\\n'`` で改行"
   "``void drawString(int x, int y, const char *)``", "``setCursor()`` してから描く"
   "``TextMetrics charMetrics(int code) const``", "1 文字の寸法 (フォントにない文字は幅 0)。整数のみで計算する"
   "``TextMetrics textMetrics(const char *) const``", "文字列の寸法 (最も幅の広い行)。整数のみで計算する"
   "``TextMetricsF charMetricsF(int code) const`` / ``TextMetricsF textMetricsF(const char *) const``", "同じ寸法を float で、描画先での大きさを加えて返す (float 演算を使う)"
   "``const TextState &textState() const``", "文字設定の取得"

.. code-block:: cpp

   struct TextMetrics {     // 整数 (フォントの寸法は整数ピクセルなので正確)
     int width;             // 進み幅 (複数行なら最も広い行)
     int height;            // 行ボックスの高さ + 2 行目以降の行送り
     int ascent;            // 行ボックス上端からベースラインまで
     int lineAdvance;       // 行送り (フォントの yAdvance)
   };

   struct TextMetricsF {    // 同じものを float で
     float width, height, ascent, lineAdvance;
     float deviceWidth;     // width と height を変換行列で拡大した、描画先での大きさ
     float deviceHeight;    // (文字の軸に沿った長さなので、回転しても変わらない)
   };

寸法は描画関数に渡す座標の単位 (変換行列を掛ける前) なので、変換行列の下でそのまま文字の配置に使えます。
``charMetrics()`` / ``textMetrics()`` は浮動小数点を使わないので、FPU のないコア (Cortex-M0+、ESP8266 など) でも軽く済みます。
描画先での大きさが必要なときだけ ``charMetricsF()`` / ``textMetricsF()`` を使います
(変換行列の列の長さを求めるため、平方根などの float 演算が入ります)。
``measureText()`` / ``charAdvance()`` / ``textHeight()`` / ``lineAdvance()`` は非推奨で、
それぞれ ``textMetrics(str).width`` / ``charMetrics(code).width`` / ``textMetrics("").height`` /
``textMetrics("").lineAdvance`` を返します。

カーソルは **行ボックスの左上** です。``setFont()`` は全グリフからベースラインより上の最大高さ (アセント) と
ボックス高さを求め、グリフはベースラインを基準に配置されます。この規約により、フォントを切り替えても
同じ ``y`` を渡せば上端が揃います。拡大や回転は変換行列で行います (整数倍率の設定はありません)。

.. code-block:: cpp

   // 右寄せ
   const char *s = "12.3 s";
   g.setFont(&ShapoSansP_s12c09a01w02);
   g.drawString(box.right() - 8 - g.textMetrics(s).width, box.y + 4, s);

   // (x, y) から 2 倍で描く
   g.pushState();
   g.translate(x, y);
   g.scale(2);
   g.drawString(0, 0, "x2");
   g.popState();

GraphicsState2D / TextState
================================================================================

.. code-block:: cpp

   struct TextState {
     const GFXfont *font;
     Color color, background;
     int cursorX, cursorY, lineStartX;
     int16_t ascent, lineHeight;
   };

   struct GraphicsState2D {
     affine2f transform;
     TextState text;
     Color colorKey;
     ucoord_t clipX, clipY, clipWidth, clipHeight;   // SHAPOGFX_COORD_BITS に応じて 8 / 16 ビット
     BlendMode blendMode;
     uint8_t opacity;
     bool colorKeyEnabled;
   };

``pushState()`` はこの構造体を丸ごとアリーナに積みます (32 ビット環境で 1 段 68 バイト、既定の 16 段で約 1.1 KB)。
``setState()`` で渡したクリップ矩形は描画先と交差され、行列の種類は判定し直されます。

実装上の注意
================================================================================

- 描画関数はフォーマットの分岐を呼び出しごと (または行ごと) に 1 回行い、ピクセルループはフォーマットごとに
  テンプレートで特殊化されています。
- 図形のブレンドの分岐は、1 行分のスパンを塗る関数 1 か所に集約されています (不透明の塗りつぶしが最初の分岐)。
  色とブレンドは呼び出しごとに 1 度だけ、描画先のネイティブ値と重みに変換されます。
- 楕円と丸角矩形は行ごとの水平範囲で表し、輪郭は「隣接する両方の行に覆われない部分 + 端点」として描かれます。
  軸に平行な楕円は整数の平方根 (1 行 1 回)、回転した楕円は二次曲線を float で解きます (1 行 1 回の平方根)。
- 回転した丸角矩形は角を弦で近似した凸多角形として塗り、輪郭はその多角形の行ごとの範囲から描きます。
- 円弧と扇形は、角度範囲の各辺を整数の方向ベクトルに 1 度丸めた半平面として表し、楕円の各行を整数除算 2 回で切り取ります。
- 文字とビットマップは 1 ビットのマスクとして、画像と同じ拡大縮小・アフィンの歩き方で描かれ、同じビットの連続がスパンになります。
  変換行列のない不透明な文字はピクセルごとに直接書きます。
