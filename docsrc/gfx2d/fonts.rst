フォント
################################################################################

ヘッダ: ``shapoco/gfx2d/fonts.hpp`` (全フォント)、``shapoco/gfx2d/gfxfont.h`` (構造体のみ)

GFXfont 形式
================================================================================

フォントは Adafruit GFX ライブラリの ``GFXfont`` 形式です。``gfxfont.h`` は Adafruit のものを同梱しており
(BSD ライセンス)、Adafruit 向けに公開されている多数のフォントや、フォント生成ツールの出力を使えます。

構造体は ``shapoco::gfx2d`` 名前空間にあり (``gfx2d::GFXglyph``、``gfx2d::GFXfont``)、インクルードガードも
独自のものなので、Adafruit GFX や LovyanGFX / M5GFX など ``GFXfont`` を定義する他のライブラリと同時に使えます。
レイアウトは Adafruit のものと同一です。

.. code-block:: cpp

   namespace shapoco::gfx2d {

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

   }  // namespace shapoco::gfx2d

同梱フォント
================================================================================

ShapoFont で生成した ShapoSans ファミリと MameSeg7 を ``include/shapoco/gfx2d/font/`` に同梱しています。
フォントオブジェクトは ``shapoco::gfx2d`` 名前空間の ``const GFXfont`` です。
各フォントのヘッダは単独でもインクルードできます。

.. csv-table::
   :header: "名前", "種類", "サイズ", "備考"

   "``ShapoSansMono_s08c07``", "等幅", "8 px (大文字 7 px)", "行送り 10"
   "``ShapoSansP_s05``", "プロポーショナル", "5 px", "行送り 6"
   "``ShapoSansP_s07c05a01``", "プロポーショナル", "7 px", "行送り 8"
   "``ShapoSansP_s08c07``", "プロポーショナル", "8 px", "行送り 10"
   "``ShapoSansP_s12c09a01w02``", "プロポーショナル", "12 px", "行送り 14"
   "``ShapoSansP_s21c16a01w03``", "プロポーショナル", "21 px", "行送り 24"
   "``ShapoSansP_s27c22a01w04``", "プロポーショナル", "27 px", "行送り 32"
   "``MameSeg7_s40c38w06``", "7 セグメント", "40 px", "行送り 48"

ShapoSans はいずれも ASCII (0x20〜0x7E) を収録しています。
MameSeg7 は ``.``、``0``〜``9``、``A``〜``F`` のみを収録しています (``.`` は送り幅 0 で直前の文字に重ねて描かれます)。

使い方
================================================================================

.. code-block:: cpp

   #include "shapoco/gfx2d/fonts.hpp"

   g.setFont(&g2::ShapoSansP_s12c09a01w02);
   g.setTextColor(g2::Colors::WHITE);           // 前景のみ (背景は透過)
   g.drawString(8, 8, "Hello");
   g.setFont(&g2::ShapoSansMono_s08c07);
   g.setTextColor(g2::Colors::YELLOW, g2::makeColor(0, 0, 128));  // 背景色付き
   g.drawString(" world");                      // カーソル位置から続けて描く
   g.pushState();                               // 拡大・回転は変換行列で
   g.translate(8, 30);
   g.scale(2);
   g.drawString(0, 0, "x2");
   g.popState();

文字描画の詳細 (カーソルの意味、行送り、計測) は :doc:`graphics2d` を参照してください。

他のフォントを使う
================================================================================

Adafruit GFX 向けのフォント (ヘッダ形式) は、``gfx2d::GFXfont`` で組み立てられるように
名前空間の中でインクルードします。

.. code-block:: cpp

   #include "shapoco/gfx2d/gfxfont.h"

   #ifndef PROGMEM
   #define PROGMEM
   #endif

   namespace shapoco {
   namespace gfx2d {
   #include "FreeSans9pt7b.h"
   }
   }

   g.setFont(&g2::FreeSans9pt7b);

.. note::

   LovyanGFX / M5GFX の ``lgfx::GFXfont`` (``fonts::FreeSans9pt7b`` など) は別の構造体で、
   ``setFont()`` には渡せません (渡そうとするとコンパイルエラーになります)。
