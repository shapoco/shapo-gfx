ボーンアニメーション (rig)
################################################################################

ヘッダ: ``shapoco/gfx2d/rig.hpp`` (``gfx2d.hpp`` からは include されません)

``shapoco::gfx2d::rig`` は、画像やベクタピクチャ (:doc:`vg`) を載せたボーンの木 (アーマチュア) を
キーフレームアニメーションで動かし、``Graphics2D`` で描画する機能です。データ (``Armature``、``Animation``) は
``static const`` でフラッシュに置け、DragonBones から :doc:`../tools/dbones2cpp` で、アニメーション付きの
SVG から :doc:`../tools/svg2cpp` で生成します。1 体ぶんの姿勢は ``rig::Instance`` が
ユーザの用意したメモリに保持します。アニメーションしないアーマチュアは ``drawBind()`` でメモリなしに
描けます。ライブラリ内でメモリを確保することはありません。

.. code-block:: cpp

   #include "shapoco/gfx2d/rig.hpp"
   #include "chara.hpp"                        // dbones2cpp の出力

   namespace rig = shapoco::gfx2d::rig;

   alignas(4) static uint8_t mem[2048];         // rig::Instance::bytes(chara::armature) 以上
   static rig::Instance inst;

   void setup() {
     inst.init(chara::armature, mem, sizeof(mem));
   }

   void frame(g2::Graphics2D &g, float seconds) {
     const rig::Animation &anim = chara::anim_walk;
     inst.pose(anim, rig::frameAt(anim, seconds));   // 姿勢を更新 (小数フレームは補間)
     g.setTransform(g2::affine2f::translation(120, 200));  // 配置は Graphics2D の変換で
     inst.draw(g);
   }

座標系は DragonBones と同じで、y 軸は下向き、角度は画面上で時計回り、変換行列は ``affine2f`` と同じ
``x' = a x + c y + tx`` の形式です。

用語
================================================================================

.. csv-table::
   :header: "名前", "意味"

   "``Armature``", "ボーン、スロット、アタッチメント、テクスチャの集合 (静的データ)"
   "``Bone``", "親を持つ木の節。ワールド変換の単位"
   "``Slot``", "描画順を持つ取り付け位置。1 本のボーンに属し、アタッチメントを 1 つ表示する"
   "``Attachment``", "スロットに付く画像またはベクタピクチャ (DragonBones の display)。スロットは複数持てて切り替えられる"
   "``Animation``", "チャネルごとのタイムライン、描画順のキー、イージングカーブの集合"
   "``Instance``", "姿勢 (ボーンのワールド変換)、スロットの状態、描画順、境界のキャッシュ"
   "``angle16_t``", "角度。1 回転 = 65536 の ``int16_t`` なので、差をとると自然に最短経路になる"
   "``scale16_t``", "倍率。Q12 (``SCALE_ONE`` = 4096 が 1.0) の ``int16_t``"

データ構造
================================================================================

すべて集合体で、ポインタと個数の組で参照します。走査は個数を超えません。

.. code-block:: cpp

   struct Bone {
     const char *name;
     float x, y;                // 親ボーンの座標系での原点 (バインドポーズ)
     angle16_t rotX, rotY;      // DragonBones の skX / skY。等しければ回転、違えばスキュー
     scale16_t scaleX, scaleY;
     uint8_t parent;            // 自分より小さい添字、または NO_PARENT
     uint8_t flags;             // 予約 (親からの回転・倍率の継承フラグ)。今は 0
   };

   struct Attachment {
     const Texture *texture;    // IMAGE: nullptr なら描かない (非対応の display の位置取り)
     Rect src;                  // texture 内の部分矩形 (アトラス)
     affine2f local;            // IMAGE: src の左上 → ボーン座標系。VECTOR: ピクチャの座標系 → ボーン座標系
     const int16_t *hull;       // 不透明部分を囲む凸多角形 (src の左上が原点の x, y の組)。nullptr: 矩形全体
     uint8_t hullCount;         // その頂点数 (0: 矩形全体)
     AttachmentKind kind;       // IMAGE または VECTOR。他 (MESH, ARMATURE, BOUNDING_BOX) は予約で、描きません
     uint8_t pad[2];
     const void *ext;           // VECTOR: const vg::Picture *。他の kind では予約
   };

   struct Slot {
     const char *name;
     const Attachment *attachments;
     uint8_t attachmentCount;
     int8_t defaultAttachment;  // -1: 非表示
     uint8_t bone;
     uint8_t alpha;             // 0..255
     BlendMode blend;           // ALPHA または ADD
     uint8_t colorR, colorG, colorB;  // スロットの色 (FEATURE_SLOT_COLOR のとき有効)
     const RectF *clip;         // クリップ矩形 (ボーン clipBone の座標系)。nullptr: なし
     uint8_t clipBone;
     uint8_t pad[3];
     float strokeWidth;         // スロットのストローク幅 (FEATURE_STROKE_WIDTH のとき有効)
   };

   struct Armature {
     const char *name;
     const Bone *bones;         // 親が子より前
     const Slot *slots;         // 基本の描画順
     uint8_t boneCount, slotCount;
     bool colorKeyEnabled;      // 画像が RGB565 + キーカラーのとき true
     Color colorKey;
     RectF bounds;              // バインドポーズの境界 (参考値)
     uint32_t signature;        // ボーン名とスロット名のハッシュ
     uint16_t features;         // 使っている機能のビット (FEATURE_SLOT_COLOR, FEATURE_STROKE_WIDTH)
   };

- ``VECTOR`` のアタッチメントは ``ext`` の ``vg::Picture`` を ``drawPicture()`` で描きます。
  ピクチャ内で ``SHAPE_FILL_CURRENT_COLOR`` などの印が付いた図形は、スロットの色 (``colorR / G / B``) で
  描かれます。SVG の ``currentColor`` と、アニメーションで変わる ``fill`` / ``stroke`` がこれになります。
- スロットの色は ``Armature::features`` に ``FEATURE_SLOT_COLOR`` が立っているときだけ読まれます
  (立っていなければ白)。画像に対しては色変換 (DragonBones の colorTransform) 用に予約で、今は適用しません。
- スロットのストローク幅 (``strokeWidth``、``FEATURE_STROKE_WIDTH`` が立っているとき。なければ 1) は、
  ピクチャ内で ``SHAPE_STROKE_CURRENT_WIDTH`` の印が付いたストロークの太さ (ピクチャ座標系) になります。
  SVG の ``stroke-width`` のアニメーションがこれになります。
- ``clip`` はスロットを ``Graphics2D`` のクリップ矩形で切り取ります。矩形はボーン ``clipBone`` の座標系で、
  そのボーンが画面上で回転していると外接矩形になります。

ボーンのローカル変換は ``a = cos(rotY)·scaleX, b = sin(rotY)·scaleX, c = -sin(rotX)·scaleY,
d = cos(rotX)·scaleY, (tx, ty) = (x, y)`` で、ワールド変換は親のワールド変換にこれを右から掛けたものです。
ボーンとスロットの添字は ``uint8_t`` で、それぞれ最大 255 個です。

アニメーション
--------------------------------------------------------------------------------

タイムラインはボーン (``TRANSLATE``、``ROTATE``、``SCALE``) またはスロット (``ATTACHMENT``、``ALPHA``、
``COLOR``、``STROKE_WIDTH``) の 1 チャネルぶんのキー列です。キーはフレーム昇順で、先頭はフレーム 0 です。

- ボーンのキーの値はバインドポーズへの **オフセット** です。位置は加算、角度は ``int16_t`` で加算
  (折り返し)、倍率はバインドの倍率に掛けます (DragonBones と同じ意味)。
- スロットのキーの値はスロットの値を置き換えます。``ATTACHMENT`` は補間しません。``COLOR`` (``ColorKey``:
  フレーム、カーブ、r, g, b) は成分ごとに不透明度と同じ式で補間します。``STROKE_WIDTH`` (``StrokeWidthKey``:
  フレーム、カーブ、float の太さ) は float で補間します。
- キーの ``curve`` は次のキーまでのイージングで、``CURVE_LINEAR``、``CURVE_STEP`` (次のキーまで保持)、
  または ``Animation::curves`` の添字です。``Curve`` は ``x = i/16`` での ``y`` を Q14 で 17 点持つ表です。
- 描画順のキー (``DrawOrderKey``) は「描画位置 → スロット」の完成した並びを持ちます (``nullptr`` は基本順)。
- ``Animation::signature`` が ``Armature::signature`` と違うアニメーションは ``pose()`` が受け付けません。
- ``RotateKey`` の ``turns`` (追加の回転数) と ``Animation::features`` は予約で、今は 0 です。

``frameAt(anim, seconds, loop = true)`` は経過秒をフレームに換算します (``seconds × frameRate``)。
``loop`` なら ``[0, duration)`` に折り返し、そうでなければ ``[0, duration]`` に収めます。
24 fps のデータを 60 Hz の表示で再生しても、小数フレームは補間されるので滑らかに動きます。

Instance
================================================================================

.. csv-table::
   :header: "関数", "説明"

   "``static size_t bytes(const Armature &)``", "``init()`` に必要なメモリ (ボーン 1 本 24 バイト、スロット 1 個 21 バイト、4 の倍数に切り上げ、境界合わせの余裕 3 バイト)"
   "``bool init(armature, memory, size)``", "メモリが足りなければ false。成功するとバインドポーズになる"
   "``void deinit()`` / ``bool isInitialized()`` / ``armature()``", "解放 (メモリは使わなくなるだけ) と状態の取得"
   "``bool pose(anim, frame, visitor = nullptr)``", "``frame`` (小数可、``[0, duration]`` に収める) の姿勢にする。別のアーマチュアのアニメーションなら false で何も変えない"
   "``void poseBind(visitor = nullptr)``", "バインドポーズにする"
   "``void draw(g)`` / ``draw(g, first, end)``", "描画 (後述)。後者は描画位置 ``[first, end)`` のスロットだけ"
   "``int drawIndexOf(slot)`` / ``int slotAt(drawIndex)``", "スロットと現在の描画位置の対応。範囲外は -1"
   "``int boneIndex(name)`` / ``int slotIndex(name)``", "名前から添字 (線形探索)。無ければ -1"
   "``const affine2f &boneTransform(bone)``", "ボーンのワールド変換 (アーマチュア座標系、配置は含まない)"
   "``attachmentOf`` / ``setAttachment`` / ``alphaOf`` / ``setAlpha`` / ``colorOf`` / ``setColor`` / ``strokeWidthOf`` / ``setStrokeWidth``", "スロットの表示中アタッチメント (-1 で非表示)、不透明度、色 (RGB。α は無視)、ストローク幅。次の ``pose()`` まで有効な上書き"
   "``RectF bounds()`` / ``RectF bounds(placement)``", "表示中のスロットを囲む矩形。アーマチュア座標系と、``placement`` を掛けた後 (保守的)"

``Instance`` は小さなハンドルで、コピーすると同じメモリを指す 2 つめのハンドルになります。

姿勢の計算
--------------------------------------------------------------------------------

``pose()`` はボーンを親から順に、バインド値に各タイムラインのオフセットを加えてローカル姿勢
(``BonePose``) を作り、``BoneVisitor`` があれば ``onBone(bone, local)`` を呼んでから、ローカル行列を
親のワールド変換に掛けます (``rotX == rotY`` なら sin/cos は 1 組)。続いてスロットの表示アタッチメント、
不透明度、色、ストローク幅を決め、スロットごとの境界 (アタッチメントの 4 隅、ピクチャならその ``bounds`` をワールド変換で
写した矩形) を更新します。
描画順はそのフレーム以前で最後のキーのもので、変わったときだけ書き換えます。

キーの補間は、フレームを挟む 2 つのキーの間の進み ``t`` (float) からイージング ``e`` (Q14 の整数) を求め、
位置は float で、角度・倍率・不透明度は整数で ``a + (b - a)·e / 16384`` を計算します。角度の差は ``int16_t``
で折り返すので、170° から -170° へのキーは 180° を通る 20° の回転になります。変換ツールの評価器
(``bin/shapogfx_dbones.py``) は同じ演算をしていて、テストの期待値になっています。

``BoneVisitor`` で姿勢を手続き的に変えられます (顔の向きを変える、など)。

.. code-block:: cpp

   class LookAt : public rig::BoneVisitor {
    public:
     int head = -1;
     rig::angle16_t turn = 0;
     void onBone(int bone, rig::BonePose &local) override {
       if (bone == head) { local.rotX += turn; local.rotY += turn; }  // 子のボーンも追従する
     }
   };

描画
--------------------------------------------------------------------------------

``draw(g)`` は ``g`` の現在の変換をアーマチュアの配置として使い、描画順に表示中のスロットを
``setTransform(配置 × ボーンのワールド変換 × アタッチメントの local)`` と
``drawImage(texture, 0, 0, src, hull, hullCount)`` (画像) または ``drawPicture(picture, スロットの色,
スロットのストローク幅)`` (ベクタ) で描きます。ライブラリに専用の画素処理はなく、回転・拡大した ``drawImage()`` /
``drawPicture()`` そのものです。``hull`` (dbones2cpp が作る、不透明部分を囲む凸多角形) は
描かれないはずの透明なピクセルだけを切り落とすので、絵は矩形全体を描いたときと同じで、走査が減るぶん速くなります。
``clip`` を持つスロットは、描く間だけ ``g`` のクリップ矩形をその矩形 (配置 × ``clipBone`` のワールド変換で
写したもの) と交差させます。

- スロットの不透明度は ``g`` の不透明度に掛かります。``ADD`` のスロットは加算で描きます (``g`` のブレンドモードが
  ``NONE`` のときを除く)。
- クリップ矩形、ブレンドモード、不透明度は ``g`` の状態に従います。境界が (配置を掛けて) クリップ矩形の外にある
  スロットは飛ばすので、帯ごとに描画しても無駄がありません。
- キーカラー出力のアーマチュアは、キーカラーのテクスチャを描く間だけそのキーカラーを設定し、ARGB4444 の
  テクスチャ (``--out-format auto`` で混在する) を描く間は外します。終わると呼び出し前の状態に戻します。
- 終わると ``g`` の変換、不透明度、ブレンドモード、カラーキー、クリップ矩形を呼び出し前に戻します
  (ステートスタックは使いません)。ベクタピクチャは ``g`` のアンチエイリアス設定で描かれます。
- ``SHAPOGFX2D_TRANSFORM=0`` の構成では何も描きません。

``rig::drawBind(g, armature)`` は ``Instance`` なしでバインドポーズ (既定のアタッチメント、不透明度、色、
基本の描画順) を同じように描きます。ボーンのワールド変換を親をたどって毎回計算するので、深い木では
``Instance`` より計算が増えますが、メモリは要りません。アニメーションしない SVG をアーマチュアとして
変換したときの描き方です (svg2cpp は、アニメーションのない SVG は既定では ``vg::Picture`` だけを出力します)。

``draw(g, first, end)`` で描画順の途中に自分の描画を挟めます。

.. code-block:: cpp

   const int k = inst.drawIndexOf(inst.slotIndex("l_hand"));
   g.setTransform(placement);
   inst.draw(g, 0, k);                                   // 手より奥
   const g2::vec2f p = (placement * inst.boneTransform(inst.boneIndex("l_hand"))).apply(30, 0);
   g.resetTransform();
   g.fillCircle(p, 20, g2::Colors::RED);                 // 手に持ったボール
   g.setTransform(placement);
   inst.draw(g, k, chara::armature.slotCount);           // 手から手前

左右反転は配置の負の倍率 (``affine2f::scaling(-1, 1)`` など) でできます。

コンパイル時オプション
================================================================================

``SHAPOGFX2D_RIG=0`` (既定 1) で機能を除去します。``init()`` は false を返し、他の関数は何もしません
(``src/gfx2d`` のみ。公開型は変わりません)。

性能の目安
================================================================================

``pose()`` はボーンごとに sin/cos 1〜2 組と 3x2 行列の積 1 回、タイムラインごとに数個のキーの探索です。
sin/cos は libm を使わず、4 分の 1 周期 257 点の Q15 表を線形補間して求めます (誤差 5e-5 未満)。
ソフトウェア浮動小数点のコア (Cortex-M0+ など) で ``sinf()`` / ``cosf()`` が 1 回数千サイクルかかるのを避けるためです。

描画はパーツ 1 枚につき回転した ``drawImage()`` 1 回で、コストはほぼ描いたピクセル数で決まります
(ARGB4444 のアトラスから RGB565 へ。RP2 では ``SHAPOGFX2D_RP2_INTERP`` のパスに乗ります)。
x86-64 で数えると、回転描画の 1 ピクセルは透明で約 25 命令、不透明で約 50 命令、半透明 (縁) で約 70 命令です。
斜めに描かれた手足の矩形は大半が透明なので、dbones2cpp が既定で付ける凸多角形 (``--hull 8``) の効果は大きく、
demorig のフレームは 320x240 で 4.11M → 3.88M 命令 (−5.5%)、640x360 で 8.34M → 7.63M (−8.6%)、
2 倍ズームで −10% になります (画面の絵は同じ)。さらに ``--fit-rotate`` で画像を回して余白を落とすと、
アトラスが 13% 減り (338 KB → 296 KB)、ズーム時のフレームがもう 1% ほど減ります (回した画像は 1 回
再サンプリングされます)。

マイコンでは、命令数と同じくらい\ **テクスチャがフラッシュ手前のキャッシュに収まるか**\ が効きます。
パーツはフレームごとに 1 回ずつ読まれるので、合計がキャッシュより大きいと毎フレーム全てをフラッシュから読み直します。
M5Stack Tab5 (L2 256 KB) は、アトラスの行が触れる 321 KB では 30 fps、画像ごとのテクスチャ (``--atlas-width 0``)
で 216 KB になると 42 fps でした。合計の減らし方は :doc:`../tools/dbones2cpp` の「性能のためのオプションの選び方」を
参照してください。

demorig のキャラクタ rgb_chan を縮尺 0.5 で変換したもの (ボーン 29、スロット 41) での計測値 (x86-64 の実行命令数):

.. csv-table::
   :header: "処理", "命令数"

   "``pose()``", "約 17,000 (うちタイムラインのないボーン計算 13,700)"
   "``draw()`` の自身のオーバーヘッド", "約 3,000 (``drawImage()`` の画素処理を除く)"
   "8 帯に分けた描画", "一括描画の 1.03 倍 (クリップ外のスキップなしでは 1.08 倍)"
   "キャラクタに掛からない帯", "約 3,000 (スキップなしでは 27,000)"

``src/gfx2d/rig.cpp`` のコードは Cortex-M33 で 8.5 KB、Cortex-M0+ で 11.0 KB です (``-O2``、sin 表を含む。
ベクタアタッチメント、クリップ、``drawBind()`` の前は 5.1 / 6.4 KB でした。使わなければリンクされません。
ベクタ描画本体 ``vg.cpp`` はピクチャを描くときだけリンクされます)。

対応していない機能
================================================================================

メッシュ変形 (FFD、ウェイト付きメッシュ)、IK、入れ子のアーマチュア、イベント、画像スロットの RGB の色変換、
追加の回転数 (``clockwise`` / ``tweenRotate``)、回転や拡大を継承しないボーン、実行時のスキン切り替え、
アニメーションのブレンド。親ボーンに非等方の倍率があると子はせん断されます (行列の積をそのまま使うため)。
変換ツールはこれらを警告して無視します。

将来の拡張のための予約
--------------------------------------------------------------------------------

上の機能を後から足せるように、各構造体には予約メンバがあります。生成ヘッダは構造体を位置指定で初期化するので、
メンバは **末尾にしか追加しません**。追加前に生成したヘッダではそのメンバは 0 になり、追加されたメンバはすべて
「0 (または列挙子 0) = 使っていない」の意味です (``-Wextra`` では ``-Wmissing-field-initializers`` の警告が出ますが、
意味は変わりません)。

- ``rig::FORMAT_VERSION`` はこうして追加したメンバの世代数です (1: 最初の配置、2: ``Slot::clip`` /
  ``clipBone``、``AttachmentKind::VECTOR``、``Channel::COLOR``、3: ``Slot::strokeWidth``、
  ``Channel::STROKE_WIDTH``)。生成ヘッダは必要な世代を
  ``static_assert`` するので、古いライブラリで新しい生成ヘッダを使うとコンパイル時に止まります。
- ``Armature::features`` / ``Animation::features`` はデータが使っている機能のビットです。今は
  ``FEATURE_SLOT_COLOR`` (スロットの色と ``COLOR`` タイムラインが有効) と ``FEATURE_STROKE_WIDTH``
  (ストローク幅と ``STROKE_WIDTH`` タイムラインが有効) が定義されています。
  ``rig::SUPPORTED_FEATURES`` にないビットが立ったデータは ``init()`` / ``pose()`` / ``drawBind()`` が
  拒否します (その機能なしで描くと作ったものと違う絵になるため)。
- ``Attachment::kind`` と ``ext``: 画像以外の display のデータの置き場。``VECTOR`` が ``vg::Picture`` に
  使っていて、メッシュ、入れ子のアーマチュア、当たり判定は予約のままです。
- ``Bone::flags``: 回転や倍率を継承しないボーンのためのフラグ。パディングに収まります。
- ``Slot::colorR / G / B``: 元は色変換用の予約 (``tintR / G / B``) でした。古いヘッダでは 0, 0, 0 になるので、
  ``FEATURE_SLOT_COLOR`` が立ったときだけ読みます。画像への適用 (色変換) は今後の課題です。
- ``RotateKey::turns``: 追加の回転数 (``clockwise`` / ``tweenRotate``)。元はパディングでした。

メッシュや IK 本体のデータ (頂点・重み・制約・そのタイムライン) は、必要になった時点で ``Armature`` /
``Animation`` の末尾に配列として足し、``Instance::bytes()`` にそのぶんのメモリを加える想定です。
