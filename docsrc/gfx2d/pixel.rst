ピクセルフォーマットと色
################################################################################

ヘッダ: ``shapoco/gfx2d/pixel.hpp`` (``shapoco/gfx2d/gfx2d.hpp`` に含まれます)

PixelFormat
================================================================================

.. code-block:: cpp

   enum class PixelFormat : uint8_t { GRAY1, RGB444, ARGB4444, RGB565BE };

.. csv-table::
   :header: "値", "ビット/px", "メモリ上の配置", "ネイティブピクセル (レジスタ上の表現)"

   "``GRAY1``", "1", "バイト内 MSB ファースト、1 = 白", "0 または 1"
   "``RGB444``", "12", "2 ピクセルを 3 バイトに: ``R1G1``, ``B1R2``, ``G2B2``", "``0x0RGB``"
   "``ARGB4444``", "16", "ネイティブ ``uint16_t``", "``0xARGB``、A = 15 で不透明"
   "``RGB565BE``", "16", "``uint16_t`` をバイトスワップして格納: byte0 = ``RRRRRGGG``, byte1 = ``GGGBBBBB``", "``RRRRRGGGGGGBBBBB`` (5/6/5)"

各行はバイト境界から始まり、行の間隔は ``stride`` バイトです。
``minStride(format, width)`` は幅 ``width`` を収める最小の stride を返します。

「ネイティブピクセル」とは、後述のカーソルや変換関数がレジスタ上で扱う 1 ピクセルの値です。
``RGB565BE`` はメモリ上ではバイトスワップされていますが、レジスタ上では通常の 5/6/5 ビット並びです。

BlendMode
--------------------------------------------------------------------------------

.. code-block:: cpp

   enum class BlendMode : uint8_t { NONE, ALPHA, ADD };

.. csv-table::
   :header: "値", "説明"

   "``NONE``", "上書き"
   "``ALPHA``", "αブレンド"
   "``ADD``", "加算合成 (飽和)"

補助関数
--------------------------------------------------------------------------------

.. csv-table::
   :header: "関数", "説明"

   "``int bitsPerPixel(PixelFormat)``", "1 ピクセルのビット数"
   "``uint32_t minStride(PixelFormat, int width)``", "最小の行ピッチ (バイト)"
   "``bool isFormatEnabled(PixelFormat)``", "コンパイル時に有効化されているか"

Color (ARGB8888)
================================================================================

.. code-block:: cpp

   using Color = uint32_t;  // 0xAARRGGBB

2D API が受け取る唯一の色型です。α = 255 で不透明、0 で完全透明です。
描画呼び出しごとに描画先のフォーマットへ変換されるので、フォーマットを意識せずに同じコードが使えます。

.. csv-table:: 定数 (``Colors`` 名前空間)
   :header: "値", "色"

   "``Colors::TRANSPARENT``", "完全透明 (0x00000000)"
   "``Colors::BLACK`` / ``WHITE`` / ``GRAY`` / ``SILVER``", "黒 / 白 / 灰 / 銀"
   "``Colors::RED`` / ``GREEN`` / ``BLUE``", "赤 / 緑 / 青"
   "``Colors::YELLOW`` / ``CYAN`` / ``MAGENTA``", "黄 / シアン / マゼンタ"

.. csv-table:: 生成と分解
   :header: "関数", "説明"

   "``Color makeColor(int r, int g, int b, int a = 255)``", "0..255 の成分から作る (範囲外はクランプ)"
   "``Color makeColorF(float r, float g, float b, float a = 1)``", "0..1 の float から作る"
   "``Color makeColorF(const colorf &)``", "``colorf`` (3D API の float 色) から作る"
   "``Color makeColorHsv(int h, int s, int v, int a = 255)``", "HSV から作る。h は度 (任意の値を 0..359 に折り返す)、s, v は 0..255"
   "``Color colorWithAlpha(Color, int a)``", "α だけ置き換える"
   "``Color lerpColor(Color a, Color b, int t)``", "補間。t は 0..256 (256 で b)"
   "``int colorA/R/G/B(Color)``", "成分の取り出し"
   "``uint32_t colorAlpha64(Color)``", "α を 0..64 に変換 (ブレンド用)"

フォーマット別の変換
================================================================================

各フォーマットのネイティブピクセルとの相互変換、および α ブレンド・加算合成の関数です。
全て ``inline`` で、ピクセル単位のループ内で使うことを想定しています。

.. csv-table::
   :header: "関数", "説明"

   "``uint32_t colorToNative(PixelFormat, Color)`` / ``Color nativeToColor(PixelFormat, uint32_t)``", "任意フォーマットとの相互変換"
   "``makeRgb565(r5, g6, b5)``, ``colorToRgb565``, ``rgb565ToColor``", "RGB565 (5/6/5)"
   "``packRgb565(float r, g, b)`` / ``packRgb565(colorf)``", "float から RGB565 へ (四捨五入)"
   "``packRgb565BE(...)``", "同上、ただし RGB565BE のメモリ順 (バイトスワップ済み) で返す"
   "``blendAlphaRgb565(dst, src, alpha64)``", "α ブレンド (α は 0..64)"
   "``addSaturateRgb565(dst, r5, g6, b5)`` / ``addSaturateRgb565(dst, src)``", "飽和加算"
   "``makeRgb444``, ``colorToRgb444``, ``rgb444ToColor``, ``rgb565ToRgb444``, ``rgb444ToRgb565``, ``blendAlphaRgb444``, ``addSaturateRgb444``", "RGB444"
   "``makeArgb4444``, ``colorToArgb4444``, ``argb4444ToColor``, ``blendAlphaArgb4444``, ``addSaturateArgb4444``", "ARGB4444。ブレンド結果の α は両者の合成 (union) になる"
   "``colorToGray1``, ``rgb565ToGray1``, ``gray1ToColor``", "GRAY1 (輝度で 2 値化)"
   "``bswap16(uint16_t)``", "バイトスワップ"
   "``fill16(uint16_t *dst, int n, uint16_t v)``", "16bit 値の高速フィル (可能な限り 32bit 書き込み)"
   "``log2Floor(int)``", "floor(log2(v))"

ピクセルカーソル
================================================================================

1 行のピクセルへ順次アクセスするための小さな構造体です。フォーマットごとに
``CursorGray1``, ``CursorRgb444``, ``CursorArgb4444``, ``CursorRgb565BE`` があり、いずれも同じインターフェイスを持ちます。
2D・3D 両方のラスタライザの部品で、アプリケーションからも利用できます。

.. code-block:: cpp

   struct CursorRgb565BE {
     void init(void *line, int x);   // 行の先頭 line のピクセル x に位置付ける
     uint32_t read() const;          // 現在のピクセル (ネイティブ表現)
     void write(uint32_t native);    // 現在のピクセルに書く
     void next();                    // 1 ピクセル進む
     void fill(int n, uint32_t v);   // n ピクセルを v で埋めて進む
   };

``RGB444`` のカーソルは奇数 x のニブル境界、``GRAY1`` のカーソルはビット位置を内部で管理します。

テンプレート補助
--------------------------------------------------------------------------------

.. csv-table::
   :header: "名前", "説明"

   "``FormatTraits<F>``", "``Cursor`` 型、``fromColor()``, ``toColor()``, ``fromRgb565()`` を提供する"
   "``blendNative<F>(dst, src, alpha64)``", "ネイティブピクセル同士の α ブレンド"
   "``addNative<F>(dst, src)``", "ネイティブピクセル同士の飽和加算"

例: 手書きのループで市松模様を書く
--------------------------------------------------------------------------------

.. code-block:: cpp

   uint16_t pixels[64 * 64];  // RGB565BE
   for (int y = 0; y < 64; y++) {
     g2::CursorRgb565BE cur;
     cur.init(pixels + y * 64, 0);
     for (int x = 0; x < 64; x++) {
       bool c = ((x >> 3) ^ (y >> 3)) & 1;
       cur.write(g2::makeRgb565(c ? 31 : 8, c ? 63 : 16, c ? 31 : 8));  // ネイティブ 5/6/5
       cur.next();
     }
   }
