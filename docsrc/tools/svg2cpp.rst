svg2cpp: SVG を C++ コードに変換する
################################################################################

``bin/svg2cpp`` は SVG ファイルを、:doc:`../gfx2d/vg` の ``vg::Picture`` (静止画) または
:doc:`../gfx2d/rig` の ``rig::Armature`` + ``rig::Animation`` (SMIL アニメーション付き) として
``static const`` データ一式のヘッダに変換します。

.. code-block:: sh

   python3 -m pip install -r bin/requirements.txt      # Pillow, numpy, fonttools
   bin/svg2cpp logo.svg logo.hpp                        # 名前空間 logo、vg::Picture
   bin/svg2cpp --scale 0.5 --font-dir /usr/share/fonts logo.svg logo.hpp
   bin/svg2cpp --rig --keep head,arm --fps 24 chara.svg chara.hpp
   bin/svg2cpp --dump chara.json chara.svg chara.hpp    # JSON の要約も書く

.. code-block:: cpp

   #include "logo.hpp"
   g.drawPicture(logo::picture);                       // 静止画

   #include "chara.hpp"                                // アニメーション付き (rig)
   rig::Instance inst;
   inst.init(chara::armature, mem, sizeof(mem));
   inst.pose(chara::anim_main, rig::frameAt(chara::anim_main, seconds));
   inst.draw(g);
   rig::drawBind(g, chara::armature);                  // インスタンスなしでバインドポーズ

ピクチャの座標系は SVG のピクセルサイズ (``width`` / ``height``、なければ ``viewBox``) × ``--scale`` で、
原点は左上です。

オプション
================================================================================

.. csv-table::
   :header: "オプション", "既定値", "説明"

   "``--namespace NS``", "入力名から", "生成物の名前空間 (``logo.svg`` → ``logo``)"
   "``--scale S``", "1", "ピクチャの縮尺"
   "``--picture``", "", "アニメーションを無視して ``vg::Picture`` 1 つにする"
   "``--rig``", "", "``rig::Armature`` とアニメーションにする (SMIL の要素があれば既定)"
   "``--keep IDS``", "", "ボーンとして残す要素の id (カンマ区切り)。``boneIndex()`` で名前から引ける"
   "``--keep-all``", "", "id を持つ全ての要素をボーンにする"
   "``--fps N``", "30", "アニメーションのフレームレート (1〜255)"
   "``--duration SECONDS``", "自動", "アニメーションの長さ。既定は繰り返すアニメーションの共通周期を、他の終わりまで伸ばしたもの (最大 60 秒)"
   "``--anim-name NAME``", "``main``", "アニメーションの名前 (``anim_<NAME>``)"
   "``--font FAMILY=PATH``", "", "font-family に使うフォントファイル (複数可)。FAMILY ``*`` は全ての族の代替"
   "``--font-dir DIR``", "", ".ttf / .otf を name テーブルの族名で探すフォルダ (複数可)"
   "``--text-font EXPR``", "``nullptr``", "アウトライン化できないテキストを描く GFXfont の C++ 式 (例 ``'&shapoco::gfx2d::ShapoSansP_s12c09a01w02'``)。nullptr は Graphics2D のフォント"
   "``--image-format F``", "``argb4444``", "画像の形式 (``argb4444`` / ``rgb565_swapped`` / ``rgb565`` / ``rgb444`` / ``gray1``)"
   "``--dither D``", "``none``", "画像のディザ (``none`` / ``diffusion`` / ``pattern``)"
   "``--dump JSON``", "", "変換結果の要約 (図形、ボーン、スロット、タイムラインのキー数) を JSON に書く"
   "``--verbose``", "", "近似についての注記を表示する"

対応している SVG
================================================================================

- **要素**: ``svg`` (``width`` / ``height`` / ``viewBox`` / ``preserveAspectRatio``)、``g``、``a``、
  ``path`` (全コマンド、絶対・相対。円弧は 3 次ベジェに変換)、``rect`` (``rx`` / ``ry``)、``circle``、``ellipse``、
  ``line``、``polyline``、``polygon``、``image`` (``data:`` URI または SVG と同じ場所のファイル。
  ``preserveAspectRatio`` 対応。形式は ``--image-format``)、``text`` / ``tspan``、``defs``、``use``
  (展開。``symbol`` の ``viewBox`` は use に大きさがあるとき)、``switch`` (``systemLanguage`` の合う最初の子)、
  ``linearGradient`` / ``radialGradient`` (``href`` の継承、``objectBoundingBox`` と ``userSpaceOnUse``、
  ``gradientTransform``、``spreadMethod``、stop-opacity。``fx`` / ``fy`` は警告して無視)、
  ``clipPath`` (``rect`` 1 つだけ。``clipPathUnits`` 対応。回転していると外接矩形にして警告)。
- **スタイル**: プレゼンテーション属性、``style`` 属性、``<style>`` (型・クラス・id・``*``・カンマ・子孫セレクタ。
  詳細度と後勝ち。他のセレクタは警告して捨てる)、継承、``currentColor`` (``SHAPE_*_CURRENT_COLOR`` フラグ)、
  CSS の色表記全般、単位 (px、pt、mm、cm、in、pc、%)、``fill-rule``、``stroke-linecap`` / ``linejoin`` /
  ``miterlimit``、静的な ``stroke-dasharray`` / ``dashoffset`` (パスを長さに沿って切り、破線ごとのサブパスにする)、
  ``visibility`` / ``display``。グループの ``opacity`` は子孫のブラシに掛けます (重なる部分は SVG と
  異なる近似。ヘッダのコメントに注記)。
- **テキスト**: ``--font`` / ``--font-dir`` で族が見つかれば fontTools でアウトライン化し、1 つの ``text`` 要素が
  1 つのパスになります (``x`` / ``y`` のリスト、``dx`` / ``dy``、``text-anchor``、``letter-spacing``、``kern``
  テーブル)。見つからなければ警告して ``vg::Text`` (``--text-font`` のビットマップフォントで描画) にします。
- **非対応** (警告して無視): ``mask``、``pattern`` (塗りは none に)、``filter``、``marker``、``foreignObject``、
  ``textPath``、SVG 形式の画像、未知の要素。

アニメーション (rig モード)
================================================================================

``animate``、``set``、``animateTransform``、``animateMotion`` のある SVG は、アニメーションする要素
(とそこに至る要素、``--keep`` の id、``--keep-all`` なら id を持つ全要素) をボーンにし、残りの静的な部分を
できるだけ少ないスロットにまとめます (連続する静的な兄弟のまとまりごとに 1 スロット。親の下の最初の
まとまりは親の名前、例えば ``root``)。ボーンの名前は要素の id です。

- 要素の静的な ``transform`` はボーン 1 本 (アニメーションのボーンが続くときは ``<id>_base``)。任意のアフィン行列を
  rig の回転・スキュー・倍率に正確に分解します (倍率は Q12 の範囲に収め、超えれば警告)。
- ``animateTransform`` ごとにバインドポーズが単位行列のボーンを 1 本足し、値そのものをキーにします
  (``additive="sum"`` の連鎖はそのまま表せる。``replace`` は同じ種類の静的な項 1 つを基準値にし、
  それ以外は警告して捨てる): ``_translate``、``_scale``、中心のある回転は ``_pivot`` + ``_rotate`` (+ ``_unpivot``)、
  ``_skewX`` / ``_skewY`` (ROTATE + SCALE のキーによる近似)。``animateMotion`` は ``_motion`` ボーンで、
  パスに沿ってフレームごとにサンプリングし (``keyPoints`` / ``keyTimes`` / ``calcMode``、直線部分は 1 キーにまとめる)、
  ``rotate`` の auto / auto-reverse / 角度は ROTATE のキーになります。連鎖の最後のボーンが要素の名前を持ちます。
- ``x`` / ``y`` / ``cx`` / ``cy`` のアニメーションは位置ボーン (``<id>_pos``、他にボーンがなければ ``<id>``) の
  TRANSLATE キー、``r`` / ``rx`` / ``ry`` / ``width`` / ``height`` は基準の大きさに対する SCALE キー
  (ストロークも拡大されるので警告)。
- ``opacity`` (図形に届く ``fill-opacity`` / ``stroke-opacity`` も) は ALPHA キー、``fill`` / ``stroke`` / ``color``
  の色は図形に ``SHAPE_*_CURRENT_COLOR`` を付けてスロットの COLOR タイムライン (スロットにつき 1 色)、
  ``stroke-width`` は図形に ``SHAPE_STROKE_CURRENT_WIDTH`` を付けてスロットの STROKE_WIDTH タイムライン
  (スロットにつき 1 つ。同じ要素の大きさのアニメーションがボーンの倍率になっている場合は、描かれる太さが
  SVG と同じになるよう、キーをその倍率で割ってフレームごとにサンプリングし、直線部分をまとめる)、
  ``visibility`` / ``display`` は ATTACHMENT キー (0 / -1)。
- タイミング: ``begin`` (オフセットのみ。イベント起動は警告して捨てる)、``dur``、``repeatCount`` / ``repeatDur``、
  ``fill`` (``remove`` は終了フレームに STEP キーで基準値へ戻る)、``calcMode`` (discrete は STEP キー、linear、
  paced、spline は ``keySplines`` を 17 点のカーブ表に)、``keyTimes``、``values`` / ``from`` / ``to`` / ``by``、
  ``additive``。``end``、``min`` / ``max``、``accumulate``、``d`` / ``points`` / ``transform``
  (``animate`` による) / 線分の端点のアニメーションは警告して捨てます。
- キーは ``--fps`` のフレームに丸め (半フレーム以内)、各タイムラインはフレーム 0 の基準値から始まり、繰り返しは
  ``--duration`` の長さに展開します。回転は最短経路の補間が崩れないよう、キーの間隔が 1/4 回転以下になるよう分割します。
- 画像のうち、ボーンになる要素の直下にあるものやアニメーションするものは、専用テクスチャの IMAGE アタッチメントになります。
  スロットのクリップはいちばん内側の ``clip-path`` の矩形で、その要素のボーンの座標系です。
- アーマチュアの ``features`` は ``FEATURE_SLOT_COLOR`` (ストローク幅のアニメーションがあれば
  ``FEATURE_STROKE_WIDTH`` も) で、ヘッダは ``rig::FORMAT_VERSION >= 3`` を要求します。

出力の構成
================================================================================

.. csv-table::
   :header: "名前", "内容"

   "``path<i>Ops``, ``path<i>Coords``, ``path<i>``", "パスの op 列、座標列、``vg::Path`` (塗り規則、制御点の範囲)"
   "``stops<i>``, ``gradient<i>`` (ストロークは ``gradient<i>s``)", "グラデーションの stop 配列と ``vg::Gradient`` (同じ stop 配列は共有)"
   "``clip<i>``, ``text<i>``, ``img<i>Data`` / ``img<i>``", "クリップ矩形、``vg::Text``、画像のピクセル配列と ``Texture`` (同じ画像は共有)"
   "``shapes[]``, ``picture``", "図形の配列と ``vg::Picture`` (ピクチャモード)"
   "``picture_<スロット名>`` とその ``path_<スロット名>_<i>`` など", "各 VECTOR アタッチメントのピクチャ (rig モード)"
   "``attachments_<スロット名>``, ``clip_<スロット名>``", "スロットのアタッチメント配列とクリップ矩形"
   "``bones``, ``slots``, ``armature``", "ボーン、スロット、``rig::Armature``"
   "``anim_<名前>``, ``animations[]``, ``ANIMATION_COUNT``", "``rig::Animation`` (キー配列、タイムライン、カーブ表がその前に並ぶ) とそのポインタ配列"

名前空間の中に ``OP_MOVE`` 〜 ``OP_CLOSE`` の定数を定義して op 列に使います。識別子にできない名前は ``_`` に
置き換えます (``name`` 文字列は SVG の id のまま)。先頭のコメントに入力ファイル、使ったオプション、グループ
不透明度の注記、警告の一覧が入ります。

制限
================================================================================

ボーン、スロット、タイムラインは各 255 個、カーブ表 253 個、65535 フレーム、パスあたり 65535 op、stop 255 個、
``use`` の入れ子 8 段。近似: グループの不透明度、キー時刻のフレームへの丸め、入れ子の不透明度の積とスキューの
補間 (区分線形)、円の 4 本の 3 次ベジェ、角度 (1/65536 回転) と倍率 (Q12) の量子化。

複雑なイラストは組み込みでの描画に向きません (ボーンとスロットの 255 個の上限もそのためです)。
アイコン、ゲージ、小さなキャラクタを想定しています。
