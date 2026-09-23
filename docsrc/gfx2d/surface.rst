Surface と Texture
################################################################################

ヘッダ: ``shapoco/gfx2d/surface.hpp``、``shapoco/gfx2d/surface_alloc.hpp`` (任意)

Texture (読み取り専用画像)
================================================================================

.. code-block:: cpp

   struct Texture {
     PixelFormat format;
     int16_t width;
     int16_t height;
     uint32_t stride;     // 行ピッチ (バイト)
     const void *pixels;

     const uint8_t *linePtr(int y) const;
   };

3D のテクスチャ、2D の ``drawImage()`` / ``drawBitmap()`` の入力です。
集成体なので Flash 上の ``const`` データとして初期化できます。

.. code-block:: cpp

   alignas(4) static const uint16_t checkerData[64 * 64] = { /* RGB565_SWAPPED */ };
   static const g2::Texture checker = {g2::PixelFormat::RGB565_SWAPPED, 64, 64, 128, checkerData};

3D レンダラで使う場合、幅と高さは 2 の冪でなければなりません。
``bin/img2cpp`` は画像ファイルからこの形式のヘッダを生成します (:doc:`../tools/img2cpp`)。

Surface (書き込み可能な画像)
================================================================================

.. code-block:: cpp

   struct Surface {
     PixelFormat format;
     int16_t width;
     int16_t height;
     uint32_t stride;
     void *pixels;

     uint8_t *linePtr(int y) const;
     Texture asTexture() const;
     operator Texture() const;      // Texture へ暗黙変換できる
   };

``Graphics2D`` の描画先、``Graphics3D::render()`` の出力先です。
``Surface`` はそのまま ``Texture`` として別の描画に使えます (オフスクリーン描画の結果を貼るなど)。

補助関数
--------------------------------------------------------------------------------

.. csv-table::
   :header: "関数", "説明"

   "``Texture makeTexture(PixelFormat, int w, int h, const void *pixels, uint32_t stride = 0)``", "stride = 0 で最小 stride"
   "``Surface makeSurface(PixelFormat, int w, int h, void *pixels, uint32_t stride = 0)``", "同上"
   "``size_t surfaceBytes(PixelFormat, int w, int h)``", "詰めて配置した場合のバイト数"

OwnedSurface (任意)
================================================================================

ヘッダ ``shapoco/gfx2d/surface_alloc.hpp`` は、ヒープにピクセルバッファを確保して自動解放する
``OwnedSurface`` を提供します。ライブラリ本体はメモリを確保しないため、このヘッダはアプリケーション側で
オフスクリーンバッファなどの寿命管理を楽にしたい場合にだけ含めてください。

.. code-block:: cpp

   #include "shapoco/gfx2d/surface_alloc.hpp"

   g2::OwnedSurface offscreen = g2::createSurface(g2::PixelFormat::RGB444, 96, 64);  // ゼロ初期化
   g2::Graphics2D g(offscreen);          // const Surface & へ暗黙変換
   g.clear(g2::Colors::BLACK);
   screen.drawImage(offscreen, 10, 10);  // Texture へも暗黙変換

- ``std::unique_ptr<uint8_t[]>`` でストレージを所有します。ムーブ可能、コピー不可です。
- ``valid()``, ``surface()``, ``format()``, ``width()``, ``height()``, ``stride()``, ``pixels()``, ``bytes()`` を持ちます。
- ムーブ元は無効 (``valid() == false``) になります。
