フォント
################################################################################

ヘッダ: ``shapoco/gfx2d/fonts.hpp`` (全フォント)、``shapoco/gfx2d/gfxfont.h`` (構造体のみ)

GFXfont 形式
================================================================================

フォントは Adafruit GFX ライブラリの ``GFXfont`` 形式です。``gfxfont.h`` は Adafruit のものを同梱しており
(BSD ライセンス)、Adafruit 向けに公開されている多数のフォントや、フォント生成ツールの出力をそのまま使えます。

.. code-block:: cpp

   typedef struct {
     uint16_t bitmapOffset;  // GFXfont::bitmap 内のオフセット
     uint8_t width, height;  // グリフのビットマップサイズ
     uint8_t xAdvance;       // カーソルの進み幅
     int8_t xOffset, yOffset;// カーソル位置からグリフ左上へのオフセット (yOffset はベースライン基準)
   } GFXglyph;

   typedef struct {
     uint8_t *bitmap;   // 全グリフのビットマップ (1bpp、MSB ファースト)
     GFXglyph *glyph;   // グリフ配列
     uint16_t first, last;  // 文字コードの範囲
     uint8_t yAdvance;  // 行送り
   } GFXfont;

同梱フォント
================================================================================

ShapoFont で生成した ShapoSans ファミリを ``include/shapoco/gfx2d/font/`` に同梱しています。
フォントオブジェクトはグローバル名前空間の ``const GFXfont`` です。

.. csv-table::
   :header: "名前", "種類", "サイズ", "備考"

   "``ShapoSansMono_s08c07``", "等幅", "8 px (大文字 7 px)", "行送り 10"
   "``ShapoSansP_s08c07``", "プロポーショナル", "8 px", "行送り 10"
   "``ShapoSansP_s12c09a01w02``", "プロポーショナル", "12 px", "行送り 14"
   "``ShapoSansP_s21c16a01w03``", "プロポーショナル", "21 px", "行送り 24"

いずれも ASCII (0x20〜0x7E) を収録しています。

使い方
================================================================================

.. code-block:: cpp

   #include "shapoco/gfx2d/fonts.hpp"

   g.setFont(&ShapoSansP_s12c09a01w02);        // 第 2 引数で整数倍の拡大 (既定 1)
   g.setTextColor(g2::Colors::WHITE);           // 前景のみ (背景は透過)
   g.drawString(8, 8, "Hello");
   g.setFont(&ShapoSansMono_s08c07, 2);         // 2 倍拡大
   g.setTextColor(g2::Colors::YELLOW, g2::makeColor(0, 0, 128));  // 背景色付き
   g.drawString("x2");                          // カーソル位置から続けて描く

文字描画の詳細 (カーソルの意味、行送り、計測) は :doc:`graphics2d` を参照してください。
