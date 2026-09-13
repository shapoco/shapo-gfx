静的シーン (Mesh / Node / Scene)
################################################################################

概要
================================================================================

モデルを「頂点配列、マテリアル、テクスチャ、ノードツリー」の集合として **全て const データ** で表し、
``putScene()`` で描くための仕組みです。glTF から ``bin/gltf2cpp`` で生成するのが主な用途ですが (:doc:`../tools/gltf2cpp`)、
手書きすることもできます。

- ヒープを使いません。全ての参照は静的記憶域のオブジェクトを指すので、寿命や所有権の問題が生じません。
- ノードごとの変換はツリーに埋め込まれていますが、``NodeVisitor`` で描画時に差し替えたり、部分木を飛ばしたりできます。

構造体
================================================================================

.. code-block:: cpp

   struct Mesh {
     const Primitive *primitives;   // 各プリミティブは自身のマテリアルを持つ
     uint16_t primitiveCount;
   };

   struct Node {
     const char *name;              // nullptr でもよい
     mat4f transform;               // 親からのローカル変換
     const Mesh *mesh;              // nullptr でもよい
     const Node *const *children;   // childCount == 0 なら nullptr でもよい
     uint16_t childCount;
   };

   struct Scene {
     const Node *const *roots;
     uint16_t rootCount;
   };

.. code-block:: cpp

   // 手書きの例: 2 つの子を持つルート
   static const g3::Node wheelL = {"WheelL", g3::mat4f::translation(-1, 0, 0), &wheelMesh, nullptr, 0};
   static const g3::Node wheelR = {"WheelR", g3::mat4f::translation(1, 0, 0), &wheelMesh, nullptr, 0};
   static const g3::Node *const carChildren[] = {&wheelL, &wheelR};
   static const g3::Node car = {"Car", g3::mat4f::identity(), &bodyMesh, carChildren, 2};
   static const g3::Node *const roots[] = {&car};
   static const g3::Scene scene = {roots, 1};

``mat4f`` は集成体なので ``{{16 個の float}}`` (列優先) でも初期化できます。gltf2cpp はこの形式で出力します。

描画関数
================================================================================

.. csv-table::
   :header: "メンバー", "説明"

   "``void putMesh(const Mesh &)``", "全プリミティブをそれぞれのマテリアルで描く"
   "``void putNode(const Node &, NodeVisitor *visitor = nullptr)``", "``pushState()`` → (visitor) → ``transform(node.transform)`` → メッシュ → 子を再帰 → ``popState()``"
   "``void putScene(const Scene &, NodeVisitor *visitor = nullptr)``", "全ルートに ``putNode()``"

状態スタック (16 段) が満杯のときは、そのノード以下を飛ばして ``Stats::nodesDropped`` に数えます。
深すぎるツリーや壊れたデータが行列スタックを壊すことはありません。

NodeVisitor
================================================================================

.. code-block:: cpp

   class NodeVisitor {
    public:
     virtual ~NodeVisitor() = default;
     // node を描く直前に呼ばれる。local はノードのローカル変換のコピーで、書き換えると
     // このフレームの描画に反映される。false を返すとノードとその子孫を描かない。
     virtual bool onNode(const Node &node, mat4f &local);
   };

ノードの名前で部品を見つけ、回転させたり隠したりする例:

.. code-block:: cpp

   class Animator : public g3::NodeVisitor {
    public:
     float angle = 0;
     bool showWheels = true;
     bool onNode(const g3::Node &node, g3::mat4f &local) override {
       if (!node.name) return true;
       if (std::strcmp(node.name, "Blades") == 0) {
         local = local * g3::mat4f::rotation(angle, {0, 0, 1});  // ローカル軸で回す
       } else if (!showWheels && std::strncmp(node.name, "Wheel", 5) == 0) {
         return false;
       }
       return true;
     }
   };

   Animator anim;
   anim.angle = t * 2.0f;
   g3d.putScene(windmill::scene, &anim);

生成されたヘッダには名前付きのノード定数 (``windmill::node_Blades`` など) も含まれるので、
部分木だけを別の場所に描く (``g3d.putNode(windmill::node_Blades)``) こともできます。

メモリ安全性について
================================================================================

静的シーンのデータは ``shared_ptr`` のような所有権管理を使いません。安全性は次の 3 点で担保しています。

1. 全ての参照先が静的記憶域にある (解放されない)。
2. 配列は必ず個数と対で表され、走査はその範囲を超えない。
3. ``putPrimitive()`` が添字を頂点数で検査し、範囲外の三角形を破棄する (``Stats::badIndices``)。

``gltf2cpp`` は生成時にも添字範囲、頂点数の上限 (65535)、テクスチャサイズを検証します。
